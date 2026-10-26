/*
 * PROJECT:     LiberNT MTP device driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     USB still image class transport: configuration, bulk and event pipes
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "wpdmtp.h"

#define NDEBUG
#include <debug.h>

static IO_COMPLETION_ROUTINE WpdMtpSyncComplete;
static IO_COMPLETION_ROUTINE WpdMtpEventComplete;

static
NTSTATUS
NTAPI
WpdMtpSyncComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_reads_opt_(_Inexpressible_("varies")) PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
NTSTATUS
WpdMtpSubmitUrb(
    _In_ PWPDMTP_DEVICE Dev,
    _Inout_ PURB Urb)
{
    PIO_STACK_LOCATION IoStack;
    LARGE_INTEGER Timeout;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    Irp = IoAllocateIrp(Dev->LowerDeviceObject->StackSize, FALSE);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoStack = IoGetNextIrpStackLocation(Irp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    IoStack->Parameters.Others.Argument1 = Urb;
    IoSetCompletionRoutine(Irp, WpdMtpSyncComplete, &Event, TRUE, TRUE, TRUE);
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    IoCallDriver(Dev->LowerDeviceObject, Irp);
    Timeout.QuadPart = -10LL * 1000 * 1000;
    while (KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, &Timeout) == STATUS_TIMEOUT)
    {
        if (Dev->Stopping)
        {
            IoCancelIrp(Irp);
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
            break;
        }
    }
    Status = Irp->IoStatus.Status;
    IoFreeIrp(Irp);
    return Status;
}

static
NTSTATUS
WpdMtpGetDescriptor(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ UCHAR Type,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG Returned)
{
    NTSTATUS Status;
    URB Urb;

    RtlZeroMemory(&Urb, sizeof(Urb));
    UsbBuildGetDescriptorRequest(&Urb, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
                                 Type, 0, 0, Buffer, NULL, Length, NULL);
    Status = WpdMtpSubmitUrb(Dev, &Urb);
    *Returned = Urb.UrbControlDescriptorRequest.TransferBufferLength;
    return Status;
}

NTSTATUS
WpdMtpUsbConfigure(
    _In_ PWPDMTP_DEVICE Dev)
{
    USB_DEVICE_DESCRIPTOR DeviceDescriptor;
    USB_CONFIGURATION_DESCRIPTOR Header;
    PUSBD_INTERFACE_LIST_ENTRY Interfaces;
    PUSB_CONFIGURATION_DESCRIPTOR Config;
    ULONG Returned, Index, Pipe, Count;
    NTSTATUS Status;
    PURB Urb;

    Status = WpdMtpGetDescriptor(Dev, USB_DEVICE_DESCRIPTOR_TYPE, &DeviceDescriptor, sizeof(DeviceDescriptor), &Returned);
    if (!NT_SUCCESS(Status) || Returned < sizeof(DeviceDescriptor))
        return NT_SUCCESS(Status) ? STATUS_DEVICE_DATA_ERROR : Status;
    Dev->VendorId = DeviceDescriptor.idVendor;
    Dev->ProductId = DeviceDescriptor.idProduct;

    Status = WpdMtpGetDescriptor(Dev, USB_CONFIGURATION_DESCRIPTOR_TYPE, &Header, sizeof(Header), &Returned);
    if (!NT_SUCCESS(Status) || Returned < sizeof(Header) || Header.wTotalLength < sizeof(Header) || !Header.bNumInterfaces)
        return NT_SUCCESS(Status) ? STATUS_DEVICE_DATA_ERROR : Status;
    Config = ExAllocatePoolWithTag(NonPagedPool, Header.wTotalLength, WPDMTP_TAG);
    if (!Config)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = WpdMtpGetDescriptor(Dev, USB_CONFIGURATION_DESCRIPTOR_TYPE, Config, Header.wTotalLength, &Returned);
    if (!NT_SUCCESS(Status) || Returned != Header.wTotalLength)
    {
        ExFreePoolWithTag(Config, WPDMTP_TAG);
        return NT_SUCCESS(Status) ? STATUS_DEVICE_DATA_ERROR : Status;
    }
    Dev->ConfigurationDescriptor = Config;

    Count = Config->bNumInterfaces;
    Interfaces = ExAllocatePoolZero(NonPagedPool, (Count + 1) * sizeof(*Interfaces), WPDMTP_TAG);
    if (!Interfaces)
        return STATUS_INSUFFICIENT_RESOURCES;
    for (Index = 0; Index < Count; Index++)
    {
        Interfaces[Index].InterfaceDescriptor =
            USBD_ParseConfigurationDescriptorEx(Config, Config, (LONG)Index, 0, -1, -1, -1);
        if (!Interfaces[Index].InterfaceDescriptor)
        {
            ExFreePoolWithTag(Interfaces, WPDMTP_TAG);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }

    Urb = USBD_CreateConfigurationRequestEx(Config, Interfaces);
    if (!Urb)
    {
        ExFreePoolWithTag(Interfaces, WPDMTP_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (Index = 0; Index < Count; Index++)
    {
        PUSBD_INTERFACE_INFORMATION Info = Interfaces[Index].Interface;

        for (Pipe = 0; Pipe < Info->NumberOfPipes; Pipe++)
            Info->Pipes[Pipe].MaximumTransferSize = WPDMTP_TRANSFER_CHUNK;
    }

    Status = WpdMtpSubmitUrb(Dev, Urb);
    if (NT_SUCCESS(Status))
    {
        Dev->ConfigurationHandle = Urb->UrbSelectConfiguration.ConfigurationHandle;
        for (Index = 0; Index < Count && !Dev->BulkIn; Index++)
        {
            PUSBD_INTERFACE_INFORMATION Info = Interfaces[Index].Interface;
            USBD_PIPE_HANDLE In = NULL, Out = NULL, Event = NULL;
            ULONG InSize = 0, OutSize = 0, EventSize = 0;

            for (Pipe = 0; Pipe < Info->NumberOfPipes; Pipe++)
            {
                PUSBD_PIPE_INFORMATION Endpoint = &Info->Pipes[Pipe];
                BOOLEAN DirectionIn = USB_ENDPOINT_DIRECTION_IN(Endpoint->EndpointAddress);

                if (Endpoint->PipeType == UsbdPipeTypeBulk && DirectionIn && !In)
                {
                    In = Endpoint->PipeHandle;
                    InSize = Endpoint->MaximumPacketSize;
                }
                else if (Endpoint->PipeType == UsbdPipeTypeBulk && !DirectionIn && !Out)
                {
                    Out = Endpoint->PipeHandle;
                    OutSize = Endpoint->MaximumPacketSize;
                }
                else if (Endpoint->PipeType == UsbdPipeTypeInterrupt && DirectionIn && !Event)
                {
                    Event = Endpoint->PipeHandle;
                    EventSize = Endpoint->MaximumPacketSize;
                }
            }
            if (In && Out)
            {
                Dev->InterfaceNumber = Info->InterfaceNumber;
                Dev->BulkIn = In;
                Dev->BulkOut = Out;
                Dev->BulkInPacketSize = InSize;
                Dev->BulkOutPacketSize = OutSize;
                Dev->InterruptIn = Event;
                Dev->InterruptPacketSize = EventSize;
            }
        }
    }
    ExFreePool(Urb);
    ExFreePoolWithTag(Interfaces, WPDMTP_TAG);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WPDMTP: select configuration failed 0x%08lx\n", Status);
        return Status;
    }
    if (!Dev->BulkIn || !Dev->BulkInPacketSize || !Dev->BulkOutPacketSize)
    {
        DPRINT1("WPDMTP: %04x:%04x has no bulk endpoint pair\n", Dev->VendorId, Dev->ProductId);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Dev->TransferBuffer = ExAllocatePoolWithTag(NonPagedPoolNx, WPDMTP_TRANSFER_CHUNK, WPDMTP_TAG);
    Dev->EventBuffer = ExAllocatePoolZero(NonPagedPoolNx, 64, WPDMTP_TAG);
    Dev->EventUrb = ExAllocatePoolZero(NonPagedPoolNx, sizeof(URB), WPDMTP_TAG);
    if (!Dev->TransferBuffer || !Dev->EventBuffer || !Dev->EventUrb)
        return STATUS_INSUFFICIENT_RESOURCES;
    DPRINT1("WPDMTP: %04x:%04x interface %u, bulk %lu/%lu bytes, event %lu bytes\n",
            Dev->VendorId, Dev->ProductId, Dev->InterfaceNumber,
            Dev->BulkInPacketSize, Dev->BulkOutPacketSize, Dev->InterruptPacketSize);
    return STATUS_SUCCESS;
}

VOID
WpdMtpUsbUnconfigure(
    _In_ PWPDMTP_DEVICE Dev)
{
    if (Dev->EventIrp)
        IoFreeIrp(Dev->EventIrp);
    Dev->EventIrp = NULL;
    if (Dev->EventUrb)
        ExFreePoolWithTag(Dev->EventUrb, WPDMTP_TAG);
    Dev->EventUrb = NULL;
    if (Dev->EventBuffer)
        ExFreePoolWithTag(Dev->EventBuffer, WPDMTP_TAG);
    Dev->EventBuffer = NULL;
    if (Dev->TransferBuffer)
        ExFreePoolWithTag(Dev->TransferBuffer, WPDMTP_TAG);
    Dev->TransferBuffer = NULL;
    if (Dev->ConfigurationDescriptor)
        ExFreePoolWithTag(Dev->ConfigurationDescriptor, WPDMTP_TAG);
    Dev->ConfigurationDescriptor = NULL;
    Dev->ConfigurationHandle = NULL;
    Dev->BulkIn = Dev->BulkOut = Dev->InterruptIn = NULL;
}

NTSTATUS
WpdMtpUsbBulk(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ BOOLEAN In,
    _Inout_updates_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG Transferred)
{
    NTSTATUS Status;
    URB Urb;

    *Transferred = 0;
    if (Dev->Stopping)
        return STATUS_DEVICE_NOT_CONNECTED;
    RtlZeroMemory(&Urb, sizeof(Urb));
    UsbBuildInterruptOrBulkTransferRequest(&Urb, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER),
                                           In ? Dev->BulkIn : Dev->BulkOut, Buffer, NULL, Length,
                                           In ? (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK) :
                                                USBD_TRANSFER_DIRECTION_OUT,
                                           NULL);
    Status = WpdMtpSubmitUrb(Dev, &Urb);
    *Transferred = Urb.UrbBulkOrInterruptTransfer.TransferBufferLength;
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WPDMTP: bulk %s of %lu bytes failed 0x%08lx (USBD 0x%08lx)\n",
                In ? "in" : "out", Length, Status, Urb.UrbHeader.Status);
        if (!Dev->Stopping && Urb.UrbHeader.Status == USBD_STATUS_STALL_PID)
            WpdMtpUsbResetPipe(Dev, In ? Dev->BulkIn : Dev->BulkOut);
    }
    return Status;
}

NTSTATUS
WpdMtpUsbResetPipe(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ USBD_PIPE_HANDLE Pipe)
{
    URB Urb;

    RtlZeroMemory(&Urb, sizeof(Urb));
    Urb.UrbHeader.Length = sizeof(struct _URB_PIPE_REQUEST);
    Urb.UrbHeader.Function = URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL;
    Urb.UrbPipeRequest.PipeHandle = Pipe;
    return WpdMtpSubmitUrb(Dev, &Urb);
}

NTSTATUS
WpdMtpUsbClassRequest(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ BOOLEAN In,
    _In_ UCHAR Request,
    _Inout_updates_bytes_opt_(Length) PVOID Buffer,
    _In_ USHORT Length)
{
    PVOID Bounce = NULL;
    NTSTATUS Status;
    URB Urb;

    if (Length)
    {
        Bounce = ExAllocatePoolZero(NonPagedPoolNx, Length, WPDMTP_TAG);
        if (!Bounce)
            return STATUS_INSUFFICIENT_RESOURCES;
        if (!In)
            RtlCopyMemory(Bounce, Buffer, Length);
    }
    RtlZeroMemory(&Urb, sizeof(Urb));
    UsbBuildVendorRequest(&Urb, URB_FUNCTION_CLASS_INTERFACE, sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST),
                          In ? (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK) : USBD_TRANSFER_DIRECTION_OUT,
                          0, Request, 0, Dev->InterfaceNumber, Bounce, NULL, Length, NULL);
    Status = WpdMtpSubmitUrb(Dev, &Urb);
    if (Bounce)
    {
        if (In && NT_SUCCESS(Status))
            RtlCopyMemory(Buffer, Bounce, min(Length, Urb.UrbControlVendorClassRequest.TransferBufferLength));
        ExFreePoolWithTag(Bounce, WPDMTP_TAG);
    }
    return Status;
}

static
VOID
WpdMtpSubmitEventRead(
    _In_ PWPDMTP_DEVICE Dev)
{
    PIO_STACK_LOCATION IoStack;

    IoReuseIrp(Dev->EventIrp, STATUS_SUCCESS);
    RtlZeroMemory(Dev->EventUrb, sizeof(URB));
    UsbBuildInterruptOrBulkTransferRequest(Dev->EventUrb, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER),
                                           Dev->InterruptIn, Dev->EventBuffer, NULL,
                                           min(Dev->InterruptPacketSize, 64),
                                           USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK, NULL);
    IoStack = IoGetNextIrpStackLocation(Dev->EventIrp);
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    IoStack->Parameters.Others.Argument1 = Dev->EventUrb;
    IoSetCompletionRoutine(Dev->EventIrp, WpdMtpEventComplete, Dev, TRUE, TRUE, TRUE);
    IoCallDriver(Dev->LowerDeviceObject, Dev->EventIrp);
}

static
NTSTATUS
NTAPI
WpdMtpEventComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_reads_opt_(_Inexpressible_("varies")) PVOID Context)
{
    PWPDMTP_DEVICE Dev = Context;
    ULONG Length = Dev->EventUrb->UrbBulkOrInterruptTransfer.TransferBufferLength;
    PMTP_CONTAINER_HEADER Header = (PMTP_CONTAINER_HEADER)Dev->EventBuffer;
    PWPDMTP_EVENT Event;
    KIRQL Irql;

    UNREFERENCED_PARAMETER(DeviceObject);
    if (NT_SUCCESS(Irp->IoStatus.Status) && Length >= sizeof(*Header) && Header->Type == MTP_CONTAINER_EVENT)
    {
        Event = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Event), WPDMTP_TAG);
        if (Event)
        {
            ULONG Index;

            Event->Code = Header->Code;
            Event->ParamCount = min((min(Length, Header->Length) - sizeof(*Header)) / 4, 3);
            for (Index = 0; Index < Event->ParamCount; Index++)
                RtlCopyMemory(&Event->Params[Index], Dev->EventBuffer + sizeof(*Header) + Index * 4, 4);
            KeAcquireSpinLock(&Dev->EventLock, &Irql);
            InsertTailList(&Dev->EventQueue, &Event->Link);
            KeReleaseSpinLock(&Dev->EventLock, Irql);
            WdfWorkItemEnqueue(Dev->EventItem);
        }
    }
    if (!Dev->Stopping && (NT_SUCCESS(Irp->IoStatus.Status) || Irp->IoStatus.Status == STATUS_DEVICE_DATA_ERROR))
    {
        WpdMtpSubmitEventRead(Dev);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }
    if (!NT_SUCCESS(Irp->IoStatus.Status) && !Dev->Stopping)
        DPRINT1("WPDMTP: event pipe stopped 0x%08lx\n", Irp->IoStatus.Status);
    InterlockedExchange(&Dev->EventPending, 0);
    KeSetEvent(&Dev->EventIdle, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
WpdMtpUsbStartEvents(
    _In_ PWPDMTP_DEVICE Dev)
{
    if (!Dev->InterruptIn)
        return STATUS_SUCCESS;
    if (!Dev->EventIrp)
    {
        Dev->EventIrp = IoAllocateIrp(Dev->LowerDeviceObject->StackSize, FALSE);
        if (!Dev->EventIrp)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeClearEvent(&Dev->EventIdle);
    InterlockedExchange(&Dev->EventPending, 1);
    WpdMtpSubmitEventRead(Dev);
    return STATUS_SUCCESS;
}

VOID
WpdMtpUsbStopEvents(
    _In_ PWPDMTP_DEVICE Dev)
{
    PLIST_ENTRY Entry;
    KIRQL Irql;

    while (Dev->EventPending)
    {
        LARGE_INTEGER Timeout;

        IoCancelIrp(Dev->EventIrp);
        Timeout.QuadPart = -10LL * 1000 * 100;
        KeWaitForSingleObject(&Dev->EventIdle, Executive, KernelMode, FALSE, &Timeout);
    }
    WdfWorkItemFlush(Dev->EventItem);
    KeAcquireSpinLock(&Dev->EventLock, &Irql);
    while (!IsListEmpty(&Dev->EventQueue))
    {
        Entry = RemoveHeadList(&Dev->EventQueue);
        ExFreePoolWithTag(CONTAINING_RECORD(Entry, WPDMTP_EVENT, Link), WPDMTP_TAG);
    }
    KeReleaseSpinLock(&Dev->EventLock, Irql);
}

VOID
NTAPI
WpdMtpEventWork(
    _In_ WDFWORKITEM WorkItem)
{
    PWPDMTP_DEVICE Dev = WpdMtpGetDevice(WdfWorkItemGetParentObject(WorkItem));
    PLIST_ENTRY Entry;
    PWPDMTP_EVENT Event;
    KIRQL Irql;

    for (;;)
    {
        KeAcquireSpinLock(&Dev->EventLock, &Irql);
        if (IsListEmpty(&Dev->EventQueue))
        {
            KeReleaseSpinLock(&Dev->EventLock, Irql);
            return;
        }
        Entry = RemoveHeadList(&Dev->EventQueue);
        KeReleaseSpinLock(&Dev->EventLock, Irql);
        Event = CONTAINING_RECORD(Entry, WPDMTP_EVENT, Link);
        WpdMtpProcessEvent(Dev, Event);
        ExFreePoolWithTag(Event, WPDMTP_TAG);
    }
}
