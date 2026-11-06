/*++
 *
 * The file contains the routines to create a device and handle ioctls
 *
-- */

#include "precomp.h"

/* event API deprecated? tidak: ZwCreateEvent hanya dideklarasikan di ntifs.h
 * (bukan ntddk.h); prototipe kita cocok signature ntoskrnl. */
NTSTATUS
ZwCreateEvent(
    OUT PHANDLE             EventHandle,
    IN ACCESS_MASK          DesiredAccess,
    IN POBJECT_ATTRIBUTES   ObjectAttributes OPTIONAL,
    IN EVENT_TYPE           EventType,
    IN BOOLEAN              InitialState
    );


#pragma NDIS_INIT_FUNCTION(FilterRegisterDevice)


_IRQL_requires_max_(PASSIVE_LEVEL)
NDIS_STATUS
FilterRegisterDevice(
    VOID
    )
{
    NDIS_STATUS            Status = NDIS_STATUS_SUCCESS;
    UNICODE_STRING         DeviceName;
    UNICODE_STRING         DeviceLinkUnicodeString;
    PDRIVER_DISPATCH       DispatchTable[IRP_MJ_MAXIMUM_FUNCTION+1];
    NDIS_DEVICE_OBJECT_ATTRIBUTES   DeviceAttribute;
    PFILTER_DEVICE_EXTENSION        FilterDeviceExtension;

    DEBUGP(DL_TRACE, "==>FilterRegisterDevice\n");

    NdisZeroMemory(DispatchTable, (IRP_MJ_MAXIMUM_FUNCTION+1) * sizeof(PDRIVER_DISPATCH));

    DispatchTable[IRP_MJ_CREATE] = FilterDispatch;
    DispatchTable[IRP_MJ_CLEANUP] = FilterDispatch;
    DispatchTable[IRP_MJ_CLOSE] = FilterDispatch;
    DispatchTable[IRP_MJ_DEVICE_CONTROL] = FilterDeviceIoControl;


    NdisInitUnicodeString(&DeviceName, NTDEVICE_STRING);
    NdisInitUnicodeString(&DeviceLinkUnicodeString, LINKNAME_STRING);

    //
    // Create a device object and register our dispatch handlers
    //
    NdisZeroMemory(&DeviceAttribute, sizeof(NDIS_DEVICE_OBJECT_ATTRIBUTES));

    DeviceAttribute.Header.Type = NDIS_OBJECT_TYPE_DEVICE_OBJECT_ATTRIBUTES;
    DeviceAttribute.Header.Revision = NDIS_DEVICE_OBJECT_ATTRIBUTES_REVISION_1;
    DeviceAttribute.Header.Size = sizeof(NDIS_DEVICE_OBJECT_ATTRIBUTES);

    DeviceAttribute.DeviceName = &DeviceName;
    DeviceAttribute.SymbolicName = &DeviceLinkUnicodeString;
    DeviceAttribute.MajorFunctions = &DispatchTable[0];
    DeviceAttribute.ExtensionSize = sizeof(FILTER_DEVICE_EXTENSION);

    Status = NdisRegisterDeviceEx(
                FilterDriverHandle,
                &DeviceAttribute,
                &NdisDeviceObject,
                &NdisFilterDeviceHandle
                );


    if (Status == NDIS_STATUS_SUCCESS)
    {
        FilterDeviceExtension = NdisGetDeviceReservedExtension(NdisDeviceObject);

        FilterDeviceExtension->Signature = 'NLWD';
        FilterDeviceExtension->Handle = FilterDriverHandle;
    }


    DEBUGP(DL_TRACE, "<==FilterRegisterDevice: %x\n", Status);

    return (Status);

}

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
FilterDeregisterDevice(
    VOID
    )

{
    if (NdisFilterDeviceHandle != NULL)
    {
        NdisDeregisterDeviceEx(NdisFilterDeviceHandle);
    }

    NdisFilterDeviceHandle = NULL;

}

_Use_decl_annotations_
NTSTATUS
FilterDispatch(
    PDEVICE_OBJECT       DeviceObject,
    PIRP                 Irp
    )
{
    PIO_STACK_LOCATION       IrpStack;
    NTSTATUS                 Status = STATUS_SUCCESS;
    BOOLEAN                  bFalse = FALSE;

    UNREFERENCED_PARAMETER(DeviceObject);

    IrpStack = IoGetCurrentIrpStackLocation(Irp);

    switch (IrpStack->MajorFunction)
    {
        case IRP_MJ_CREATE:
            break;

        case IRP_MJ_CLEANUP:
            break;

        case IRP_MJ_CLOSE:
        {
            /* v2: lepas mapping shm + event yang owned file ini (FILE_OBJECT
             * masih valid di titik ini). NlShmUnmap menangani keduanya. */
            bFalse = FALSE;
            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
            {
                PLIST_ENTRY cLink;
                for (cLink = FilterModuleList.Flink; cLink != &FilterModuleList; cLink = cLink->Flink)
                {
                    PMS_FILTER pF = CONTAINING_RECORD(cLink, MS_FILTER, FilterModuleLink);
                    if (pF->NlShmOwnerFile == IrpStack->FileObject ||
                        pF->NlShmEventFile == IrpStack->FileObject)
                    {
                        NlShmUnmap(pF);
                    }
                }
            }
            FILTER_RELEASE_LOCK(&FilterListLock, bFalse);
            break;
        }

        default:
            break;
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return Status;
}

_Use_decl_annotations_
NTSTATUS
FilterDeviceIoControl(
    PDEVICE_OBJECT        DeviceObject,
    PIRP                  Irp
    )
{
    PIO_STACK_LOCATION          IrpSp;
    NTSTATUS                    Status = STATUS_SUCCESS;
    PFILTER_DEVICE_EXTENSION    FilterDeviceExtension;
    PUCHAR                      InputBuffer;
    PUCHAR                      OutputBuffer;
    ULONG                       InputBufferLength, OutputBufferLength;
    PLIST_ENTRY                 Link;
    PUCHAR                      pInfo;
    ULONG                       InfoLength = 0;
    PMS_FILTER                  pFilter = NULL;
    BOOLEAN                     bFalse = FALSE;


    UNREFERENCED_PARAMETER(DeviceObject);


    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    if (IrpSp->FileObject == NULL)
    {
        return(STATUS_UNSUCCESSFUL);
    }


    FilterDeviceExtension = (PFILTER_DEVICE_EXTENSION)NdisGetDeviceReservedExtension(DeviceObject);

    ASSERT(FilterDeviceExtension->Signature == 'NLWD');

    Irp->IoStatus.Information = 0;

    switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
    {

        case IOCTL_FILTER_RESTART_ALL:
            break;

        case IOCTL_FILTER_RESTART_ONE_INSTANCE:
            InputBuffer = OutputBuffer = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
            InputBufferLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;

            /* cari by nama di bawah list lock + ref (NdisFRestartFilter
             * dipanggil di luar kunci) */
            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
            for (Link = FilterModuleList.Flink; Link != &FilterModuleList; Link = Link->Flink)
            {
                PMS_FILTER pF = CONTAINING_RECORD(Link, MS_FILTER, FilterModuleLink);
                if (InputBufferLength >= (ULONG)pF->FilterModuleName.Length &&
                    NdisEqualMemory(InputBuffer,
                                    pF->FilterModuleName.Buffer,
                                    pF->FilterModuleName.Length))
                {
                    InterlockedIncrement((volatile LONG *)&pF->RefCount);
                    pFilter = pF;
                    break;
                }
            }
            FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

            if (pFilter != NULL)
            {
                NdisFRestartFilter(pFilter->FilterHandle);
                filterDereferenceModule(pFilter);
                pFilter = NULL;
            }

            break;

        case IOCTL_FILTER_ENUMERATE_ALL_INSTANCES:

            InputBuffer = OutputBuffer = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
            InputBufferLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
            OutputBufferLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;


            pInfo = OutputBuffer;

            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);

            Link = FilterModuleList.Flink;

            while (Link != &FilterModuleList)
            {
                pFilter = CONTAINING_RECORD(Link, MS_FILTER, FilterModuleLink);


                InfoLength += (pFilter->FilterModuleName.Length + sizeof(USHORT));

                if (InfoLength <= OutputBufferLength)
                {
                    *(PUSHORT)pInfo = pFilter->FilterModuleName.Length;
                    NdisMoveMemory(pInfo + sizeof(USHORT),
                                   (PUCHAR)(pFilter->FilterModuleName.Buffer),
                                   pFilter->FilterModuleName.Length);

                    pInfo += (pFilter->FilterModuleName.Length + sizeof(USHORT));
                }

                Link = Link->Flink;
            }

            FILTER_RELEASE_LOCK(&FilterListLock, bFalse);
            if (InfoLength <= OutputBufferLength)
            {

                Status = NDIS_STATUS_SUCCESS;
            }
            //
            // Buffer is small
            //
            else
            {
                Status = STATUS_BUFFER_TOO_SMALL;
            }
            break;


        case NL_IOCTL_VERSION:
        {
            NL_VERSION v;
            v.Magic = NL_ABI_MAGIC;
            v.Version = NL_ABI_VERSION;
            InfoLength = sizeof(NL_VERSION);
            if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength >= InfoLength)
            {
                NdisMoveMemory(Irp->AssociatedIrp.SystemBuffer, &v, sizeof(v));
                Status = STATUS_SUCCESS;
            }
            else
            {
                Status = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        }

        case NL_IOCTL_ENUM_ADPT:
        {
            PNL_ADAPTER pas = (PNL_ADAPTER)Irp->AssociatedIrp.SystemBuffer;
            ULONG OutLen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
            ULONG idx = 0;

            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
            for (Link = FilterModuleList.Flink; Link != &FilterModuleList; Link = Link->Flink)
            {
                PMS_FILTER pF = CONTAINING_RECORD(Link, MS_FILTER, FilterModuleLink);
                PNL_ADAPTER a;
                ULONG cch;

                if ((ULONG)OutLen < (idx + 1) * sizeof(NL_ADAPTER)) { break; }
                a = &pas[idx];

                NdisZeroMemory(a, sizeof(NL_ADAPTER));
                a->Index = idx;
                a->Flags = (pF->State == FilterRunning) ? NL_FLAG_RUNNING : 0;
                a->Medium = pF->NlMedium;
                NdisMoveMemory(a->Mac, pF->NlMac, 6);

                cch = pF->FilterModuleName.Length / sizeof(WCHAR);
                if (cch > NL_IFNAME_MAX - 1) { cch = NL_IFNAME_MAX - 1; }
                NdisMoveMemory(a->IfName, pF->FilterModuleName.Buffer, cch * sizeof(WCHAR));
                a->IfName[cch] = 0;

                idx++;
            }
            FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

            InfoLength = idx * sizeof(NL_ADAPTER);
            Status = STATUS_SUCCESS;
            break;
        }

        case NL_IOCTL_SET_MODE:
        {
            PNL_MODE pm = (PNL_MODE)Irp->AssociatedIrp.SystemBuffer;
            ULONG InLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;

            if (InLen != sizeof(NL_MODE))
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
            {
                PLIST_ENTRY l;
                BOOLEAN found = FALSE;
                ULONG ordinal = 0;   /* harus cocok urutan ENUM_ADPT */
                for (l = FilterModuleList.Flink; l != &FilterModuleList; l = l->Flink)
                {
                    PMS_FILTER pF = CONTAINING_RECORD(l, MS_FILTER, FilterModuleLink);
                    if (ordinal == pm->AdapterIndex)
                    {
                        InterlockedExchange((volatile LONG *)&pF->NlMode, (LONG)pm->Flags);
                        found = TRUE;
                        break;
                    }
                    ordinal++;
                }
                FILTER_RELEASE_LOCK(&FilterListLock, bFalse);
                Status = (found ? STATUS_SUCCESS : STATUS_NOT_FOUND);
            }
            break;
        }

        case NL_IOCTL_STATS:
        {
            PNL_STATS ps = (PNL_STATS)Irp->AssociatedIrp.SystemBuffer;
            LONG64 rx = 0, tx = 0, capRx = 0, capTx = 0, inj = 0;
            ULONG drop = 0;
            ULONG units = 0;
            ULONG wantIdx = 0;
            BOOLEAN perAdapter = (IrpSp->Parameters.DeviceIoControl.InputBufferLength >= sizeof(UINT32));

            if (perAdapter && Irp->AssociatedIrp.SystemBuffer != NULL)
            {
                wantIdx = *(volatile UINT32 *)Irp->AssociatedIrp.SystemBuffer;
            }

            inj = NlInj;
            drop += (ULONG)NlBlocked;   /* policy block (global) + ring drop */

            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
            {
                PMS_FILTER pS;
                if (perAdapter)
                {
                    pS = NlFilterByIdxLocked(wantIdx);
                    if (pS != NULL)
                    {
                        rx += pS->NlRxBytes;
                        tx += pS->NlTxBytes;
                        capRx += pS->NlCapRx;
                        capTx += pS->NlCapTx;
                        drop += pS->NlTxDrop + pS->NlRxDrop;
                        units++;
                    }
                }
                else
                {
                    PLIST_ENTRY lAll;
                    for (lAll = FilterModuleList.Flink; lAll != &FilterModuleList; lAll = lAll->Flink)
                    {
                        PMS_FILTER pF = CONTAINING_RECORD(lAll, MS_FILTER, FilterModuleLink);
                        rx += pF->NlRxBytes;
                        tx += pF->NlTxBytes;
                        capRx += pF->NlCapRx;
                        capTx += pF->NlCapTx;
                        drop += pF->NlTxDrop + pF->NlRxDrop;
                        units++;
                    }
                }
            }
            FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

            InfoLength = sizeof(NL_STATS);
            if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength >= InfoLength)
            {
                NdisZeroMemory(ps, sizeof(NL_STATS));
                ps->RxTotal = (UINT64)rx;
                ps->TxTotal = (UINT64)tx;
                ps->RxCaptured = (UINT64)capRx;
                ps->TxCaptured = (UINT64)capTx;
                ps->Injected = (UINT64)inj;
                ps->Dropped = (UINT64)drop;
                ps->ActiveUnits = units;
                ps->AbiVersion = NL_ABI_VERSION;
                Status = STATUS_SUCCESS;
            }
            else
            {
                Status = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        }

        case NL_IOCTL_READ_RING:
        {
            PUCHAR ob = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
            ULONG OutLen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
            ULONG InLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
            ULONG Max;
            ULONG cnt;
            ULONG wantIdx = 0;
            PMS_FILTER pSel = NULL;

            if (OutLen < sizeof(ULONG))
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            /* in (opsional): [u32 adapter_index]; tanpa in = instance pertama */
            if (InLen >= sizeof(UINT32) && ob != NULL)
            {
                wantIdx = *(volatile UINT32 *)ob;
            }

            /* in (opsional): [u32 adapter_index]; tanpa in = instance pertama.
             * resolve ORDINAL list (M6+): sama dengan ENUM/SET_MODE/MIRROR. */
            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
            if (InLen >= sizeof(UINT32))
            {
                pSel = NlFilterByIdxLocked(wantIdx);
            }
            else
            {
                pSel = (FilterModuleList.Flink != &FilterModuleList)
                     ? CONTAINING_RECORD(FilterModuleList.Flink, MS_FILTER,
                                         FilterModuleLink)
                     : NULL;
            }
            /* tahan lifetime di luar kunci (drain dipanggil pasca release) */
            if (pSel != NULL)
            {
                InterlockedIncrement((volatile LONG *)&pSel->RefCount);
            }
            FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

            if (pSel == NULL)
            {
                *(ULONG *)ob = 0;
                InfoLength = sizeof(ULONG);
                Status = STATUS_SUCCESS;
                break;
            }

            Max = (OutLen - sizeof(ULONG)) / sizeof(NL_RING_ENTRY);
            cnt = NlDrainAll(pSel, (PNL_RING_ENTRY)(ob + sizeof(ULONG)), Max);
            filterDereferenceModule(pSel);

            *(ULONG *)ob = cnt;
            InfoLength = sizeof(ULONG) + cnt * sizeof(NL_RING_ENTRY);
            Status = STATUS_SUCCESS;
            break;
        }

        case NL_IOCTL_INJECT:
        {
            PNL_INJECT pj = (PNL_INJECT)Irp->AssociatedIrp.SystemBuffer;
            ULONG InLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
            NDIS_STATUS st;

            if (InLen < sizeof(NL_INJECT) || pj->Len == 0 || pj->Len > NL_FRAME_MAX)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            st = NlInjectTx(pj->Data, pj->Len);
            Status = (st == NDIS_STATUS_SUCCESS) ? STATUS_SUCCESS
                   : (st == NDIS_STATUS_RESOURCES) ? STATUS_INSUFFICIENT_RESOURCES
                   : STATUS_INVALID_PARAMETER;
            InfoLength = 0;
            break;
        }

        case NL_IOCTL_POLICY:
        {
            PNL_POLICY pp = (PNL_POLICY)Irp->AssociatedIrp.SystemBuffer;
            ULONG InLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;

            if (InLen != sizeof(NL_POLICY) || pp->Count > NL_POLICY_MAX_RULES)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            NlPolicySet(pp);
            Status = STATUS_SUCCESS;
            InfoLength = 0;
            break;
        }

        case NL_IOCTL_POLICY_EX:
        {
            PNL_POLICY_EX pp = (PNL_POLICY_EX)Irp->AssociatedIrp.SystemBuffer;
            ULONG InLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;

            if (InLen != sizeof(NL_POLICY_EX) || pp == NULL ||
                pp->Count > NL_POLICY_MAX_RULES)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            NlPolicyExSet(pp);
            Status = STATUS_SUCCESS;
            InfoLength = 0;
            break;
        }

        case NL_IOCTL_DELAYSTAT:
        {
            PNL_DELAY_STAT pd = (PNL_DELAY_STAT)Irp->AssociatedIrp.SystemBuffer;
            ULONG OutLen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;

            if (Irp->AssociatedIrp.SystemBuffer == NULL ||
                OutLen < sizeof(NL_DELAY_STAT))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            pd->Pending = (ULONG64)NlDelayCount;
            pd->Fired = NlDelayFired;
            InfoLength = sizeof(NL_DELAY_STAT);
            Status = STATUS_SUCCESS;
            break;
        }

        /* v8 (M8): status antrean delay per rule — read-only snapshot. */
        case NL_IOCTL_DELAYSTAT_EX:
        {
            PNL_DELAY_STAT_EX pe = (PNL_DELAY_STAT_EX)Irp->AssociatedIrp.SystemBuffer;
            ULONG OutLen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
            ULONG i;

            if (Irp->AssociatedIrp.SystemBuffer == NULL ||
                OutLen < sizeof(NL_DELAY_STAT_EX))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            pe->Pending = (ULONG64)NlDelayCount;
            pe->Fired = NlDelayFired;
            for (i = 0; i < NL_POLICY_MAX_RULES; i++)
            {
                pe->FiredRule[i] = NlDelayFiredRule[i];
            }
            InfoLength = sizeof(NL_DELAY_STAT_EX);
            Status = STATUS_SUCCESS;
            break;
        }

        case NL_IOCTL_PROGRAM:
        {
            PNL_VM_PROG pv = (PNL_VM_PROG)Irp->AssociatedIrp.SystemBuffer;
            ULONG InLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;

            if (InLen != sizeof(NL_VM_PROG) || pv == NULL)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            NlVmSet(pv);
            Status = STATUS_SUCCESS;
            InfoLength = 0;
            break;
        }

        /* v6 (M5): pasang/cabut mirror adapter→adapter */
        case NL_IOCTL_MIRROR:
        {
            PNL_MIRROR_CFG pm = (PNL_MIRROR_CFG)Irp->AssociatedIrp.SystemBuffer;
            ULONG InLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;

            if (InLen != sizeof(NL_MIRROR_CFG) || pm == NULL)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            NlMirrorSet(pm);
            Status = STATUS_SUCCESS;
            InfoLength = 0;
            break;
        }

        /* v6 (M5): inject TX ke adapter tertentu (REDIRECT) */
        case NL_IOCTL_INJECT_TO:
        {
            PNL_INJECT_TO pj = (PNL_INJECT_TO)Irp->AssociatedIrp.SystemBuffer;
            ULONG InLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
            NDIS_STATUS st;

            if (InLen < sizeof(NL_INJECT_TO) || pj->Len == 0 || pj->Len > NL_FRAME_MAX)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            st = NlInjectTo(pj->DstIndex, pj->Data, pj->Len);
            Status = (st == NDIS_STATUS_SUCCESS) ? STATUS_SUCCESS
                   : (st == NDIS_STATUS_RESOURCES) ? STATUS_INSUFFICIENT_RESOURCES
                   : (st == NDIS_STATUS_INVALID_STATE) ? STATUS_DEVICE_NOT_READY
                   : STATUS_INVALID_PARAMETER;
            InfoLength = 0;
            break;
        }

        case NL_IOCTL_DEBUG:
        {
            PNL_DEBUG pd = (PNL_DEBUG)Irp->AssociatedIrp.SystemBuffer;
            ULONG OutLen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;

            if (Irp->AssociatedIrp.SystemBuffer == NULL ||
                OutLen < sizeof(NL_DEBUG))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            NlDebugDump(pd);
            InfoLength = sizeof(NL_DEBUG);
            Status = STATUS_SUCCESS;
            break;
        }

        case NL_IOCTL_INJECT_RX:
        {
            PNL_INJECT pj = (PNL_INJECT)Irp->AssociatedIrp.SystemBuffer;
            ULONG InLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
            NDIS_STATUS st;

            if (InLen < sizeof(NL_INJECT) || pj->Len == 0 || pj->Len > NL_FRAME_MAX)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            st = NlInjectRx(pj->Data, pj->Len);
            Status = (st == NDIS_STATUS_SUCCESS) ? STATUS_SUCCESS
                   : (st == NDIS_STATUS_RESOURCES) ? STATUS_INSUFFICIENT_RESOURCES
                   : (st == NDIS_STATUS_INVALID_STATE) ? STATUS_DEVICE_NOT_READY
                   : STATUS_INVALID_PARAMETER;
            InfoLength = 0;
            break;
        }

        case NL_IOCTL_RULEHITS:
        {
            PNL_RULEHITS ph = (PNL_RULEHITS)Irp->AssociatedIrp.SystemBuffer;
            ULONG OutLen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
            ULONG i;

            if (Irp->AssociatedIrp.SystemBuffer == NULL ||
                OutLen < sizeof(NL_RULEHITS))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            NdisZeroMemory(ph, sizeof(NL_RULEHITS));
            for (i = 0; i < NL_POLICY_MAX_RULES; i++)
            {
                ph->Hits[i] = NlRuleHits[i];
            }
            ph->BlockTx = NlBlockedTx;
            ph->BlockRx = NlBlockedRx;
            InfoLength = sizeof(NL_RULEHITS);
            Status = STATUS_SUCCESS;
            break;
        }

        case NL_IOCTL_MAP_RING:
        {
            PNL_MAP_RING pm = (PNL_MAP_RING)Irp->AssociatedIrp.SystemBuffer;
            ULONG InLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
            PLIST_ENTRY ml;
            NL_SHM_PREP prep;
            ULONG ordinal = 0;   /* urutan harus cocok ENUM_ADPT/SET_MODE */

            if (InLen != sizeof(NL_MAP_RING) || pm == NULL)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            /* cari adapter + taruh referensi (MmProbeAndLockPages tidak boleh
             * berada di bawah spinlock, dan module bisa detach walaupun
             * referensi masih bertahan). */
            pFilter = NULL;
            Status = STATUS_DEVICE_NOT_CONNECTED;
            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
            for (ml = FilterModuleList.Flink; ml != &FilterModuleList; ml = ml->Flink)
            {
                PMS_FILTER pF = CONTAINING_RECORD(ml, MS_FILTER, FilterModuleLink);
                if (ordinal == pm->AdapterIndex)
                {
                    InterlockedIncrement((volatile LONG *)&pF->RefCount);
                    pFilter = pF;
                    Status = STATUS_SUCCESS;
                    break;
                }
                ordinal++;
            }
            FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

            if (pFilter == NULL)
            {
                Status = STATUS_NOT_FOUND;
                InfoLength = 0;
                break;
            }

            /* bekerja di PASSIVE tanpa kunci apa pun */
            if (!NlShmPrepare(pm->UserVa, pm->Len, &prep))
            {
                filterDereferenceModule(pFilter);
                Status = STATUS_INVALID_PARAMETER;
                InfoLength = 0;
                break;
            }

            if (NlShmInstall(pFilter, &prep, IrpSp->FileObject))
            {
                Status = STATUS_SUCCESS;
            }
            else
            {
                /* sudah ter-map (busy) atau instance sudah lepas list —
                 * prep yang tak terpasang wajib dibersihkan. */
                NlShmPrepareCleanup(&prep);
                Status = STATUS_DEVICE_BUSY;
            }

            filterDereferenceModule(pFilter);
            pFilter = NULL;
            InfoLength = 0;
            break;
        }

        case NL_IOCTL_UNMAP_RING:
        {
            ULONG InLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
            UINT32 wantIdx;
            PLIST_ENTRY ul;
            ULONG ordinal = 0;

            wantIdx = 0;
            if (InLen >= sizeof(UINT32) && Irp->AssociatedIrp.SystemBuffer != NULL)
            {
                wantIdx = *(volatile UINT32 *)Irp->AssociatedIrp.SystemBuffer;
            }

            Status = STATUS_NOT_FOUND;

            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
            for (ul = FilterModuleList.Flink; ul != &FilterModuleList; ul = ul->Flink)
            {
                PMS_FILTER pF = CONTAINING_RECORD(ul, MS_FILTER, FilterModuleLink);
                if (ordinal == wantIdx)
                {
                    /* pemilik mapping (jika ada) membuat: UNMAP dari file
                     * lain ditolak */
                    if (pF->NlShmOwnerFile != NULL && pF->NlShmOwnerFile != IrpSp->FileObject)
                    {
                        Status = STATUS_ACCESS_DENIED;
                    }
                    else
                    {
                        /* unmap langsung di bawah list lock: seluruh operasi
                         * gantinya (MmUnlock/deref event) diperbolehkan di
                         * DISPATCH_LEVEL yang diangkat spinlock ini */
                        NlShmUnmap(pF);
                        Status = STATUS_SUCCESS;
                    }
                    break;
                }
                ordinal++;
            }
            FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

            InfoLength = 0;
            break;
        }

        case NL_IOCTL_GET_EVENT:
        {
            PNL_EVENT pe = (PNL_EVENT)Irp->AssociatedIrp.SystemBuffer;
            ULONG InLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
            ULONG OutLen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
            HANDLE evUser = NULL;
            PVOID evObj = NULL, evOld;
            OBJECT_ATTRIBUTES oa;
            KIRQL OldIrql;
            PLIST_ENTRY el;
            ULONG ordinal = 0;
            UINT32 wantIdx = 0;
            PMS_FILTER pSel = NULL;

            if (InLen < sizeof(UINT32) || OutLen < sizeof(NL_EVENT) ||
                Irp->AssociatedIrp.SystemBuffer == NULL)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            wantIdx = *(volatile UINT32 *)Irp->AssociatedIrp.SystemBuffer;

            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
            for (el = FilterModuleList.Flink; el != &FilterModuleList; el = el->Flink)
            {
                PMS_FILTER pF = CONTAINING_RECORD(el, MS_FILTER, FilterModuleLink);
                if (ordinal == wantIdx)
                {
                    pSel = pF;
                    break;
                }
                ordinal++;
            }
            /* tahan lifetime module di luar kunci list (detach minggir). */
            if (pSel != NULL)
            {
                InterlockedIncrement((volatile LONG *)&pSel->RefCount);
            }
            FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

            if (pSel == NULL)
            {
                Status = STATUS_NOT_FOUND;
                break;
            }

            /* CREATE event di ring konteks caller (PASSIVE): handle user
             * berlaku di proses app, objek kernelnya memiliki ref kita. */
            InitializeObjectAttributes(&oa, NULL, 0, NULL, NULL);
            Status = ZwCreateEvent(&evUser,
                                   EVENT_MODIFY_STATE | SYNCHRONIZE,
                                   &oa,
                                   SynchronizationEvent,
                                   FALSE);
            if (!NT_SUCCESS(Status))
            {
                filterDereferenceModule(pSel);
                break;
            }

            Status = ObReferenceObjectByHandle(evUser,
                                               EVENT_MODIFY_STATE,
                                               *ExEventObjectType,
                                               UserMode,
                                               &evObj,
                                               NULL);
            if (!NT_SUCCESS(Status))
            {
                ZwClose(evUser);
                filterDereferenceModule(pSel);
                break;
            }

            /* ref kernel dipasang di bawah ring lock (capture membaca
             * NlShmEvent di bawah lock yang sama); ref event lama dilepas
             * setelah swap agar CALL GET_EVENT ulang tidak bocor. */
            KeAcquireSpinLock(&pSel->NlTxRingLock, &OldIrql);
            evOld = pSel->NlShmEvent;
            pSel->NlShmEvent = (PKEVENT)evObj;
            pSel->NlShmEventFile = IrpSp->FileObject;
            KeReleaseSpinLock(&pSel->NlTxRingLock, OldIrql);
            if (evOld != NULL)
            {
                ObDereferenceObject(evOld);
            }

            filterDereferenceModule(pSel);

            pe->Handle = (ULONG64)(ULONG_PTR)evUser;
            InfoLength = sizeof(NL_EVENT);
            Status = STATUS_SUCCESS;
            break;
        }

        case NL_IOCTL_CLEAR:
        {
            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
            for (Link = FilterModuleList.Flink; Link != &FilterModuleList; Link = Link->Flink)
            {
                PMS_FILTER pF = CONTAINING_RECORD(Link, MS_FILTER, FilterModuleLink);
                InterlockedExchange64(&pF->NlRxBytes, 0);
                InterlockedExchange64(&pF->NlTxBytes, 0);
                InterlockedExchange64(&pF->NlCapRx, 0);
                InterlockedExchange64(&pF->NlCapTx, 0);
                InterlockedExchange((volatile LONG *)&pF->NlTxDrop, 0);
                InterlockedExchange((volatile LONG *)&pF->NlRxDrop, 0);
            }
            FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

            Status = STATUS_SUCCESS;
            InfoLength = 0;
            break;
        }

        default:
            break;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = InfoLength;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return Status;


}




