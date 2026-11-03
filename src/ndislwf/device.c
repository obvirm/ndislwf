/*++
 *
 * The file contains the routines to create a device and handle ioctls
 *
-- */

#include "precomp.h"


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

        FilterDeviceExtension->Signature = 'FTDR';
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

    UNREFERENCED_PARAMETER(DeviceObject);

    IrpStack = IoGetCurrentIrpStackLocation(Irp);

    switch (IrpStack->MajorFunction)
    {
        case IRP_MJ_CREATE:
            break;

        case IRP_MJ_CLEANUP:
            break;

        case IRP_MJ_CLOSE:
            break;

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

    ASSERT(FilterDeviceExtension->Signature == 'FTDR');

    Irp->IoStatus.Information = 0;

    switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
    {

        case IOCTL_FILTER_RESTART_ALL:
            break;

        case IOCTL_FILTER_RESTART_ONE_INSTANCE:
            InputBuffer = OutputBuffer = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
            InputBufferLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;

            pFilter = filterFindFilterModule (InputBuffer, InputBufferLength);

            if (pFilter == NULL)
            {

                break;
            }

            NdisFRestartFilter(pFilter->FilterHandle);

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

            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
            for (Link = FilterModuleList.Flink; Link != &FilterModuleList; Link = Link->Flink)
            {
                PMS_FILTER pF = CONTAINING_RECORD(Link, MS_FILTER, FilterModuleLink);
                if (perAdapter && pF->NlIdx != wantIdx)
                {
                    continue;
                }
                rx += pF->NlRxBytes;
                tx += pF->NlTxBytes;
                capRx += pF->NlCapRx;
                capTx += pF->NlCapTx;
                drop += pF->NlTxDrop + pF->NlRxDrop;
                units++;
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
            PLIST_ENTRY l2;

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

            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
            for (l2 = FilterModuleList.Flink; l2 != &FilterModuleList; l2 = l2->Flink)
            {
                PMS_FILTER pF = CONTAINING_RECORD(l2, MS_FILTER, FilterModuleLink);
                if (InLen >= sizeof(UINT32))
                {
                    if (pF->NlIdx == wantIdx)
                    {
                        pSel = pF;
                        break;
                    }
                }
                else
                {
                    pSel = pF;
                    break;
                }
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


_IRQL_requires_max_(DISPATCH_LEVEL)
PMS_FILTER
filterFindFilterModule(
    _In_reads_bytes_(BufferLength)
         PUCHAR                   Buffer,
    _In_ ULONG                    BufferLength
    )
{

   PMS_FILTER              pFilter;
   PLIST_ENTRY             Link;
   BOOLEAN                  bFalse = FALSE;

   FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);

   Link = FilterModuleList.Flink;

   while (Link != &FilterModuleList)
   {
       pFilter = CONTAINING_RECORD(Link, MS_FILTER, FilterModuleLink);

       if (BufferLength >= pFilter->FilterModuleName.Length)
       {
           if (NdisEqualMemory(Buffer, pFilter->FilterModuleName.Buffer, pFilter->FilterModuleName.Length))
           {
               FILTER_RELEASE_LOCK(&FilterListLock, bFalse);
               return pFilter;
           }
       }

       Link = Link->Flink;
   }

   FILTER_RELEASE_LOCK(&FilterListLock, bFalse);
   return NULL;
}




