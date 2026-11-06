/* shimprobe -- verifikasi shim ndisapi.dll atas nldriver.
 * Urutan: open -> IsDriverLoaded -> enum -> set mode listen 2 arah ->
 * udpTx anchor -> ReadPackets drain -> SendPacketToAdapter -> ReadPackets lagi.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>

#define ADAPTER_LIST_SIZE   32
#define ADAPTER_NAME_SIZE   256
#define ETHER_ADDR_LENGTH   6
#define MAX_ETHER_FRAME     1514
#define PACKET_FLAG_ON_SEND     0x00000001
#define PACKET_FLAG_ON_RECEIVE  0x00000002
#define MSTCP_FLAG_SENT_LISTEN  0x00000004
#define MSTCP_FLAG_RECV_LISTEN  0x00000008

#pragma pack(push, 1)
typedef struct { void* Flink; void* Blink; } IBLE;
typedef union { void* h; IBLE q; } IBHDR;
typedef struct {
    IBHDR hdr;
    unsigned long devFlags, len, flags, vlan, filterId;
    unsigned long rsv[4];
    unsigned char buf[MAX_ETHER_FRAME];
} IB;
#pragma pack(pop)

typedef HANDLE  (__stdcall* pOpen)(const char*);
typedef VOID    (__stdcall* pClose)(HANDLE);
typedef BOOL    (__stdcall* pBool1)(HANDLE);
typedef BOOL    (__stdcall* pBool)(HANDLE, void*);
typedef DWORD   (__stdcall* pDw)(HANDLE);
typedef BOOL    (__stdcall* pSend)(HANDLE, void*);
typedef BOOL    (__stdcall* pConv)(const char*, char*, unsigned long);

static void udpTx(int n)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { return; }
    {
        SOCKET sd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sd != INVALID_SOCKET)
        {
            struct sockaddr_in sa;
            const char pay[20] = "nlshim-probe";
            int i;
            BOOL br = TRUE;
            (void)setsockopt(sd, SOL_SOCKET, SO_BROADCAST, (char*)&br, sizeof br);
            memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET;
            sa.sin_port = htons(47701);
            sa.sin_addr.s_addr = 0xFFFFFFFFu;
            for (i = 0; i < n; ++i)
            {
                (void)sendto(sd, pay, sizeof pay, 0, (SOCKADDR*)&sa, sizeof sa);
            }
            closesocket(sd);
        }
    }
    WSACleanup();
}

static int FAIL = 0;
#define CHK(name, cond)  \
    do { if (cond) printf("[PASS] %s\n", name); else { printf("[FAIL] %s\n", name); ++FAIL; } } while (0)

int main(int argc, char** argv)
{
    HINSTANCE dll;
    pOpen openFn;
    pClose closeFn;
    pBool1 loadedFn;
    pBool enumFn, setModeFn, readFn;
    pSend sendFn, sendMstcpFn;
    HANDLE h;
    HANDLE hnd0;
    unsigned char listBuf[16384];   /* TCP_AdapterList = 8836 B */
    unsigned char* pkList;
    void** pkBufArr;
    unsigned char reqBuf[65536];    /* cap hdr + num*ptr + 32 IB */
    unsigned long count;
    int batchSize = 32;
    int i, nRead;

    (void)argc; (void)argv;
    {
        BOOL ok;
        unsigned long num;
        unsigned char* ibs;
        char name0[ADAPTER_NAME_SIZE + 1];
        pConv convFn;

        dll = LoadLibraryA("ndisapi.dll");
        CHK("load ndisapi.dll", dll != NULL);
        if (dll == NULL) { printf("EXIT 1\n"); return 1; }

        openFn = (pOpen)GetProcAddress(dll, "OpenFilterDriver");
        closeFn = (pClose)GetProcAddress(dll, "CloseFilterDriver");
        loadedFn = (pBool1)GetProcAddress(dll, "IsDriverLoaded");
        enumFn = (pBool)GetProcAddress(dll, "GetTcpipBoundAdaptersInfo");
        setModeFn = (pBool)GetProcAddress(dll, "SetAdapterMode");
        readFn = (pBool)GetProcAddress(dll, "ReadPackets");
        sendFn = (pSend)GetProcAddress(dll, "SendPacketToAdapter");
        sendMstcpFn = (pSend)GetProcAddress(dll, "SendPacketToMstcp");
        convFn = (pConv)GetProcAddress(dll, "ConvertWindows2000AdapterName");
        CHK("GetProcAddress lengkap",
            openFn && closeFn && loadedFn && enumFn && setModeFn && readFn && sendFn && sendMstcpFn && convFn);

        h = openFn("nlwf");
        CHK("OpenFilterDriver", h != NULL && h != INVALID_HANDLE_VALUE);

        ok = loadedFn(h);
        CHK("IsDriverLoaded", ok);

        memset(listBuf, 0, sizeof listBuf);
        ok = enumFn(h, listBuf);
        count = *(unsigned long*)listBuf;
        printf("      GetTcpipBoundAdaptersInfo ret=%d count=%lu\n", ok, count);
        CHK("enum: count >= 1", ok && count >= 1);
        if (!ok || count < 1) { closeFn(h); printf("EXIT 2\n"); return 2; }

        hnd0 = *(void**)(listBuf + 4 + 32 * ADAPTER_NAME_SIZE);
        memcpy(name0, listBuf + 4, ADAPTER_NAME_SIZE);
        name0[ADAPTER_NAME_SIZE] = 0;

        {
            char friendly[300];
            ok = convFn(name0, friendly, sizeof friendly);
            printf("      adapter0=%s friendly=%s handle=%p\n", name0, friendly, hnd0);
            CHK("ConvertWindows2000AdapterName", ok);
        }

        {
            /* ADAPTER_MODE = {/HANDLE 8, DWORD 4} = 12 B */
            unsigned char mode[12];
            *(void**)(mode + 0) = hnd0;
            *(unsigned long*)(mode + 8) = MSTCP_FLAG_SENT_LISTEN | MSTCP_FLAG_RECV_LISTEN;
            ok = setModeFn(h, mode);
            CHK("SetAdapterMode listen 2 arah", ok);
        }

        udpTx(20);
        Sleep(400);

        /* dbg-pack layout ETH_M_REQUEST: {HANDLE 8; num 4; succ 4; ptr[num]} */
        num = (unsigned long)batchSize;
        memset(reqBuf, 0, sizeof reqBuf);
        pkList = reqBuf;
        pkBufArr = (void**)(reqBuf + 16);  /* cap hdr 16 B; ptr[num] mengikuti */
        ibs = reqBuf + 16 + num * 8;
        for (i = 0; i < (int)num; ++i)
        {
            pkBufArr[i] = ibs + (size_t)i * 1600;
        }
        *(void**)(pkList + 0) = hnd0;
        *(unsigned long*)(pkList + 8) = num;
        *(unsigned long*)(pkList + 12) = 0;
        ok = readFn(h, pkList);
        nRead = (int)*(unsigned long*)(pkList + 12);
        printf("      ReadPackets ret=%d succ=%d\n", ok, nRead);
        CHK("ReadPackets >= 1 setelah udpTx", ok && nRead >= 1);
        if (nRead >= 1)
        {
            const unsigned char* ib0 = (const unsigned char*)pkBufArr[0];
            /* IB: hdr 16 + dev 16..+4... => len @ +20, flags@+24 */
            unsigned long len0 = *(const unsigned long*)(ib0 + 20);
            unsigned long df0 = *(const unsigned long*)(ib0 + 16);
            printf("      IB0 devFlags=0x%lX len=%lu\n", df0, len0);
            CHK("IB0 len>0", len0 > 0 && len0 <= MAX_ETHER_FRAME);
            CHK("IB0 devFlags valid",
                df0 == PACKET_FLAG_ON_SEND || df0 == PACKET_FLAG_ON_RECEIVE);
        }

        /* redirect: kirim ke adapter handle nol via shim nl_inject_to */
        {
            unsigned char sendReq[16];
            unsigned char* ibS = (unsigned char*)pkBufArr[0];
            *(void**)(sendReq) = hnd0;
            *(void**)(sendReq + 8) = ibS;
            ok = sendFn(h, sendReq);
            printf("      SendPacketToAdapter ret=%d\n", ok);
            CHK("SendPacketToAdapter", ok);
        }

        {
            unsigned char sendReq[16];
            unsigned char* ibS = (unsigned char*)pkBufArr[0];
            *(void**)(sendReq) = hnd0;
            *(void**)(sendReq + 8) = ibS;
            ok = sendMstcpFn(h, sendReq);
            printf("      SendPacketToMstcp ret=%d\n", ok);
            CHK("SendPacketToMstcp", ok);
        }
    }
    closeFn(h);
    printf("RINGKASAN shimprobe: fail=%d\n", FAIL);
    printf("EXIT %d\n", FAIL ? 2 : 0);
    return FAIL ? 2 : 0;
}
