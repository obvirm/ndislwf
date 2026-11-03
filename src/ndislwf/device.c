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


        case VB_IOCTL_VERSION:
        {
            VB_VERSION v;
            v.Magic = VB_ABI_MAGIC;
            v.Version = VB_ABI_VERSION;
            InfoLength = sizeof(VB_VERSION);
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

        case VB_IOCTL_ENUM_ADPT:
        {
            PVB_ADAPTER pas = (PVB_ADAPTER)Irp->AssociatedIrp.SystemBuffer;
            ULONG OutLen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
            ULONG idx = 0;

            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
            for (Link = FilterModuleList.Flink; Link != &FilterModuleList; Link = Link->Flink)
            {
                PMS_FILTER pF = CONTAINING_RECORD(Link, MS_FILTER, FilterModuleLink);
                PVB_ADAPTER a;
                ULONG cch;

                if ((ULONG)OutLen < (idx + 1) * sizeof(VB_ADAPTER)) { break; }
                a = &pas[idx];

                NdisZeroMemory(a, sizeof(VB_ADAPTER));
                a->Index = idx;
                a->Flags = (pF->State == FilterRunning) ? VB_FLAG_RUNNING : 0;

                cch = pF->FilterModuleName.Length / sizeof(WCHAR);
                if (cch > VB_IFNAME_MAX - 1) { cch = VB_IFNAME_MAX - 1; }
                NdisMoveMemory(a->IfName, pF->FilterModuleName.Buffer, cch * sizeof(WCHAR));
                a->IfName[cch] = 0;

                idx++;
            }
            FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

            InfoLength = idx * sizeof(VB_ADAPTER);
            Status = STATUS_SUCCESS;
            break;
        }

        case VB_IOCTL_SET_MODE:
        {
            PVB_MODE pm = (PVB_MODE)Irp->AssociatedIrp.SystemBuffer;
            ULONG InLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;

            if (InLen != sizeof(VB_MODE))
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
                        InterlockedExchange((volatile LONG *)&pF->VbMode, (LONG)pm->Flags);
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

        case VB_IOCTL_STATS:
        {
            PVB_STATS ps = (PVB_STATS)Irp->AssociatedIrp.SystemBuffer;
            LONG64 rx = 0, tx = 0, capRx = 0, capTx = 0, inj = 0;
            ULONG units = 0;

            capRx = VbCapRx;
            capTx = VbCapTx;
            inj = VbInj;

            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
            for (Link = FilterModuleList.Flink; Link != &FilterModuleList; Link = Link->Flink)
            {
                PMS_FILTER pF = CONTAINING_RECORD(Link, MS_FILTER, FilterModuleLink);
                rx += pF->VbRxBytes;
                tx += pF->VbTxBytes;
                units++;
            }
            FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

            InfoLength = sizeof(VB_STATS);
            if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength >= InfoLength)
            {
                NdisZeroMemory(ps, sizeof(VB_STATS));
                ps->RxTotal = (UINT64)rx;
                ps->TxTotal = (UINT64)tx;
                ps->RxCaptured = (UINT64)capRx;
                ps->TxCaptured = (UINT64)capTx;
                ps->Injected = (UINT64)inj;
                ps->Dropped = (UINT64)InterlockedOr((volatile LONG *)&VbTxDrop, 0)
                            + (UINT64)InterlockedOr((volatile LONG *)&VbRxDrop, 0);
                ps->ActiveUnits = units;
                ps->AbiVersion = VB_ABI_VERSION;
                Status = STATUS_SUCCESS;
            }
            else
            {
                Status = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        }

        case VB_IOCTL_READ_RING:
        {
            PUCHAR ob = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
            ULONG OutLen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
            ULONG Max;
            ULONG cnt;

            if (OutLen < sizeof(ULONG))
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            Max = (OutLen - sizeof(ULONG)) / sizeof(VB_RING_ENTRY);
            cnt = VbDrainAll((PVB_RING_ENTRY)(ob + sizeof(ULONG)), Max);

            *(ULONG *)ob = cnt;
            InfoLength = sizeof(ULONG) + cnt * sizeof(VB_RING_ENTRY);
            Status = STATUS_SUCCESS;
            break;
        }

        case VB_IOCTL_INJECT:
        {
            PVB_INJECT pj = (PVB_INJECT)Irp->AssociatedIrp.SystemBuffer;
            ULONG InLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
            NDIS_STATUS st;

            if (InLen < sizeof(VB_INJECT) || pj->Len == 0 || pj->Len > VB_FRAME_MAX)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            st = VbInjectTx(pj->Data, pj->Len);
            Status = (st == NDIS_STATUS_SUCCESS) ? STATUS_SUCCESS
                   : (st == NDIS_STATUS_RESOURCES) ? STATUS_INSUFFICIENT_RESOURCES
                   : STATUS_INVALID_PARAMETER;
            InfoLength = 0;
            break;
        }

        case VB_IOCTL_CLEAR:
        {
            FILTER_ACQUIRE_LOCK(&FilterListLock, bFalse);
            for (Link = FilterModuleList.Flink; Link != &FilterModuleList; Link = Link->Flink)
            {
                PMS_FILTER pF = CONTAINING_RECORD(Link, MS_FILTER, FilterModuleLink);
                InterlockedExchange64(&pF->VbRxBytes, 0);
                InterlockedExchange64(&pF->VbTxBytes, 0);
            }
            FILTER_RELEASE_LOCK(&FilterListLock, bFalse);

            VbTxDrop = 0;
            VbRxDrop = 0;
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




