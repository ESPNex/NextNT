/*
 * PROJECT:     LiberNT MTP device driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Windows Portable Devices driver for USB MTP devices
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "wpdmtp.h"

#define NDEBUG
#include <debug.h>

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD WpdMtpEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE WpdMtpEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE WpdMtpEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY WpdMtpEvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT WpdMtpEvtD0Exit;
static EVT_WDF_DEVICE_FILE_CREATE WpdMtpEvtFileCreate;
static EVT_WDF_FILE_CLEANUP WpdMtpEvtFileCleanup;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL WpdMtpEvtIoDeviceControl;

static
VOID
WpdMtpProcessRequest(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN Drain);

static
VOID
WpdMtpDrainPending(
    _In_ PWPDMTP_DEVICE Dev)
{
    WDFREQUEST Request;

    for (;;)
    {
        WpdMtpLock(Dev);
        if (Dev->ActiveTransfer)
        {
            WpdMtpUnlock(Dev);
            return;
        }
        WpdMtpUnlock(Dev);
        if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Dev->PendingQueue, &Request)))
            return;
        WpdMtpProcessRequest(Dev, Request, FALSE);
    }
}

static
VOID
WpdMtpProcessRequest(
    _In_ PWPDMTP_DEVICE Dev,
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN Drain)
{
    WDF_REQUEST_PARAMETERS Parameters;
    PWPD_COLLECTION Params = NULL, Results = NULL;
    PWPDMTP_FILE File;
    PVOID InBuffer, OutBuffer;
    size_t InSize, OutSize;
    ULONG Code, Size;
    BOOLEAN Idle;
    NTSTATUS Status;

    WDF_REQUEST_PARAMETERS_INIT(&Parameters);
    WdfRequestGetParameters(Request, &Parameters);
    Code = Parameters.Parameters.DeviceIoControl.IoControlCode;
    if (Code != IOCTL_WPD_MESSAGE_READWRITE_ACCESS && Code != IOCTL_WPD_MESSAGE_READ_ACCESS)
    {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    Status = WdfRequestRetrieveInputBuffer(Request, 1, &InBuffer, &InSize);
    if (NT_SUCCESS(Status))
        Status = WdfRequestRetrieveOutputBuffer(Request, 1, &OutBuffer, &OutSize);
    if (NT_SUCCESS(Status))
        Status = WpdDeserialize(InBuffer, (ULONG)min(InSize, MAXULONG), &Params);
    if (!NT_SUCCESS(Status))
    {
        WdfRequestComplete(Request, Status);
        return;
    }
    File = WpdMtpGetFile(WdfRequestGetFileObject(Request));

    WpdMtpLock(Dev);
    if (WpdMtpRequestBlocked(Dev, Params))
    {
        Status = WdfRequestForwardToIoQueue(Request, Dev->PendingQueue);
        WpdMtpUnlock(Dev);
        WpdCollectionFree(Params);
        if (!NT_SUCCESS(Status))
            WdfRequestComplete(Request, Status);
        return;
    }
    Status = WpdMtpDispatch(Dev, File, Code == IOCTL_WPD_MESSAGE_READWRITE_ACCESS, Params,
                            (ULONG)min(OutSize, MAXULONG), &Results);
    Idle = !Dev->ActiveTransfer;
    WpdMtpUnlock(Dev);
    WpdCollectionFree(Params);

    if (NT_SUCCESS(Status))
    {
        Size = WpdSerializedSize(Results);
        if (Size > OutSize)
        {
            Status = STATUS_BUFFER_TOO_SMALL;
            Size = 0;
        }
        else
        {
            WpdSerialize(Results, OutBuffer, Size);
        }
        WpdCollectionFree(Results);
        WdfRequestCompleteWithInformation(Request, Status, Size);
    }
    else
    {
        WdfRequestComplete(Request, Status);
    }
    if (Drain && Idle)
        WpdMtpDrainPending(Dev);
}

static
VOID
NTAPI
WpdMtpEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(IoControlCode);
    WpdMtpProcessRequest(WpdMtpGetDevice(WdfIoQueueGetDevice(Queue)), Request, TRUE);
}

static
VOID
NTAPI
WpdMtpEvtFileCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject)
{
    UNREFERENCED_PARAMETER(Device);
    RtlZeroMemory(WpdMtpGetFile(FileObject), sizeof(WPDMTP_FILE));
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

static
VOID
NTAPI
WpdMtpEvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject)
{
    PWPDMTP_DEVICE Dev = WpdMtpGetDevice(WdfFileObjectGetDevice(FileObject));

    WpdMtpLock(Dev);
    WpdMtpReleaseFile(Dev, WpdMtpGetFile(FileObject));
    WpdMtpUnlock(Dev);
    WpdMtpDrainPending(Dev);
}

static
NTSTATUS
NTAPI
WpdMtpEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PWPDMTP_DEVICE Dev = WpdMtpGetDevice(Device);

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    Dev->LowerDeviceObject = WdfDeviceWdmGetAttachedDevice(Device);
    Dev->Stopping = FALSE;
    return WpdMtpUsbConfigure(Dev);
}

static
NTSTATUS
NTAPI
WpdMtpEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PWPDMTP_DEVICE Dev = WpdMtpGetDevice(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    WpdMtpFreeCache(Dev);
    WpdMtpUsbUnconfigure(Dev);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
WpdMtpEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PWPDMTP_DEVICE Dev = WpdMtpGetDevice(Device);
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(PreviousState);
    Dev->Stopping = FALSE;
    WpdMtpLock(Dev);
    Status = WpdMtpLoadDevice(Dev);
    WpdMtpUnlock(Dev);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WPDMTP: %04x:%04x session setup failed 0x%08lx\n", Dev->VendorId, Dev->ProductId, Status);
        return Status;
    }
    return WpdMtpUsbStartEvents(Dev);
}

static
NTSTATUS
NTAPI
WpdMtpEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PWPDMTP_DEVICE Dev = WpdMtpGetDevice(Device);

    UNREFERENCED_PARAMETER(TargetState);
    WpdMtpLock(Dev);
    WpdMtpReleaseAllContexts(Dev);
    WpdMtpCloseSession(Dev);
    WpdMtpForgetAll(Dev);
    Dev->Stopping = TRUE;
    WpdMtpUnlock(Dev);
    WpdMtpUsbStopEvents(Dev);
    WdfIoQueuePurgeSynchronously(Dev->PendingQueue);
    WdfIoQueueStart(Dev->PendingQueue);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
WpdMtpEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Callbacks;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_DEVICE_PNP_CAPABILITIES PnpCapabilities;
    WDF_FILEOBJECT_CONFIG FileConfig;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDF_WORKITEM_CONFIG WorkItemConfig;
    PWPDMTP_DEVICE Dev;
    WDFDEVICE Device;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Callbacks);
    Callbacks.EvtDevicePrepareHardware = WpdMtpEvtPrepareHardware;
    Callbacks.EvtDeviceReleaseHardware = WpdMtpEvtReleaseHardware;
    Callbacks.EvtDeviceD0Entry = WpdMtpEvtD0Entry;
    Callbacks.EvtDeviceD0Exit = WpdMtpEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Callbacks);

    WDF_FILEOBJECT_CONFIG_INIT(&FileConfig, WpdMtpEvtFileCreate, WDF_NO_EVENT_CALLBACK, WpdMtpEvtFileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WPDMTP_FILE);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &FileConfig, &Attributes);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WPDMTP_DEVICE);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&PnpCapabilities);
    PnpCapabilities.SurpriseRemovalOK = WdfTrue;
    WdfDeviceSetPnpCapabilities(Device, &PnpCapabilities);

    Dev = WpdMtpGetDevice(Device);
    Dev->Device = Device;
    KeInitializeEvent(&Dev->Lock, SynchronizationEvent, TRUE);
    KeInitializeEvent(&Dev->EventIdle, NotificationEvent, TRUE);
    KeInitializeSpinLock(&Dev->EventLock);
    InitializeListHead(&Dev->EventQueue);
    InitializeListHead(&Dev->Storages);
    InitializeListHead(&Dev->Objects);
    InitializeListHead(&Dev->PropDescs);
    InitializeListHead(&Dev->Contexts);

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoDeviceControl = WpdMtpEvtIoDeviceControl;
    QueueConfig.PowerManaged = WdfFalse;
    Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
        return Status;
    WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchManual);
    QueueConfig.PowerManaged = WdfFalse;
    Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &Dev->PendingQueue);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_WORKITEM_CONFIG_INIT(&WorkItemConfig, WpdMtpEventWork);
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Status = WdfWorkItemCreate(&WorkItemConfig, &Attributes, &Dev->EventItem);
    if (!NT_SUCCESS(Status))
        return Status;

    return WdfDeviceCreateDeviceInterface(Device, &GUID_DEVINTERFACE_WPD, NULL);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, WpdMtpEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
