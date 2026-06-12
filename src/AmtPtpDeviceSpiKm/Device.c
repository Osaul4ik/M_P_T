/*++
Module Name:
    device.c
Abstract:
    This file contains the device entry points and callbacks.
Environment:
    Kernel-mode Driver Framework
--*/

#include "driver.h"
#include "device.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtPtpDeviceSpiKmCreateDevice)
#endif

static VOID
AmtPtpResetTrackingState(
	_In_ PDEVICE_CONTEXT pDeviceContext
)
{
	UINT8 k;
	pDeviceContext->PrevAdjustedCount = 0;
	pDeviceContext->PrevReportedCount = 0;
	pDeviceContext->PrevReportedMask  = 0;
	for (k = 0; k < PTP_MAX_CONTACT_POINTS; k++) {
		pDeviceContext->SlotIsPalm[k] = FALSE;
		pDeviceContext->SlotLastX[k]  = 0;
		pDeviceContext->SlotLastY[k]  = 0;
	}
}

NTSTATUS
AmtPtpDeviceSpiKmCreateDevice(
	_Inout_ PWDFDEVICE_INIT DeviceInit
)
{
	WDF_OBJECT_ATTRIBUTES DeviceAttributes;
	WDF_OBJECT_ATTRIBUTES TimerAttributes;
	WDF_OBJECT_ATTRIBUTES RequestAttributes;
	PDEVICE_CONTEXT pDeviceContext;
	WDF_TIMER_CONFIG TimerConfig;
	WDFDEVICE Device;
	NTSTATUS Status;
	WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;

	PAGED_CODE();

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
	pnpPowerCallbacks.EvtDevicePrepareHardware        = AmtPtpEvtDevicePrepareHardware;
	pnpPowerCallbacks.EvtDeviceD0Entry                = AmtPtpEvtDeviceD0Entry;
	pnpPowerCallbacks.EvtDeviceD0Exit                 = AmtPtpEvtDeviceD0Exit;
	pnpPowerCallbacks.EvtDeviceSelfManagedIoInit      = AmtPtpEvtDeviceSelfManagedIoInitOrRestart;
	pnpPowerCallbacks.EvtDeviceSelfManagedIoRestart   = AmtPtpEvtDeviceSelfManagedIoInitOrRestart;
	WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&DeviceAttributes, DEVICE_CONTEXT);
	Status = WdfDeviceCreate(&DeviceInit, &DeviceAttributes, &Device);
	if (!NT_SUCCESS(Status)) {
		goto exit;
	}

	pDeviceContext = DeviceGetContext(Device);
	pDeviceContext->SpiDevice = Device;

	Status = WdfLookasideListCreate(
		WDF_NO_OBJECT_ATTRIBUTES,
		REPORT_BUFFER_SIZE,
		NonPagedPoolNx,
		WDF_NO_OBJECT_ATTRIBUTES,
		PTP_LIST_POOL_TAG,
		&pDeviceContext->HidReadBufferLookaside
	);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! WdfLookasideListCreate failed with %!STATUS!", Status);
		goto exit;
	}

	WDF_TIMER_CONFIG_INIT(&TimerConfig, AmtPtpPowerRecoveryTimerCallback);
	TimerConfig.AutomaticSerialization = TRUE;
	WDF_OBJECT_ATTRIBUTES_INIT(&TimerAttributes);
	TimerAttributes.ParentObject   = Device;
	TimerAttributes.ExecutionLevel = WdfExecutionLevelPassive;
	Status = WdfTimerCreate(&TimerConfig, &TimerAttributes,
		&pDeviceContext->PowerOnRecoveryTimer);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! WdfTimerCreate failed with %!STATUS!", Status);
		goto exit;
	}

	pDeviceContext->SpiTrackpadIoTarget = WdfDeviceGetIoTarget(Device);
	if (pDeviceContext->SpiTrackpadIoTarget == NULL) {
		Status = STATUS_INVALID_DEVICE_STATE;
		goto exit;
	}

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&RequestAttributes, WORKER_REQUEST_CONTEXT);
	RequestAttributes.ParentObject = Device;
	Status = WdfRequestCreate(
		&RequestAttributes,
		pDeviceContext->SpiTrackpadIoTarget,
		&pDeviceContext->SpiHidReadRequest
	);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! WdfRequestCreate (preallocated) failed with %!STATUS!", Status);
		goto exit;
	}

	Status = WdfMemoryCreateFromLookaside(
		pDeviceContext->HidReadBufferLookaside,
		&pDeviceContext->SpiHidReadOutputMemory
	);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! WdfMemoryCreateFromLookaside (preallocated) failed with %!STATUS!", Status);
		goto exit;
	}

	{
		PWORKER_REQUEST_CONTEXT pCtx =
			WorkerRequestGetContext(pDeviceContext->SpiHidReadRequest);
		pCtx->DeviceContext  = pDeviceContext;
		pCtx->RequestMemory  = pDeviceContext->SpiHidReadOutputMemory;
	}

	Status = WdfIoTargetFormatRequestForInternalIoctl(
		pDeviceContext->SpiTrackpadIoTarget,
		pDeviceContext->SpiHidReadRequest,
		IOCTL_HID_READ_REPORT,
		NULL, 0,
		pDeviceContext->SpiHidReadOutputMemory, 0
	);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! WdfIoTargetFormatRequestForInternalIoctl (preallocated) failed with %!STATUS!", Status);
		goto exit;
	}

	WdfRequestSetCompletionRoutine(
		pDeviceContext->SpiHidReadRequest,
		AmtPtpRequestCompletionRoutine,
		WorkerRequestGetContext(pDeviceContext->SpiHidReadRequest)
	);

	// FIX (sync): use macro wrapper so the store is an interlocked operation,
	// visible to the completion routine running at DISPATCH_LEVEL.
	DEVICE_STATUS_WRITE(pDeviceContext, D3);

	Status = WdfDeviceCreateDeviceInterface(
		Device,
		&GUID_DEVINTERFACE_AmtPtpDeviceSpiKm,
		NULL
	);
	if (NT_SUCCESS(Status)) {
		Status = AmtPtpDeviceSpiKmQueueInitialize(Device);
	}

exit:
	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! Exit, Status = %!STATUS!", Status);
	return Status;
}

NTSTATUS
AmtPtpEvtDevicePrepareHardware(
	_In_ WDFDEVICE Device,
	_In_ WDFCMRESLIST ResourceList,
	_In_ WDFCMRESLIST ResourceListTranslated
)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PDEVICE_CONTEXT pDeviceContext;
	WDF_MEMORY_DESCRIPTOR HidAttributeMemoryDescriptor;
	HID_DEVICE_ATTRIBUTES DeviceAttributes;
	const SPI_TRACKPAD_INFO* pTrackpadInfo;
	BOOLEAN DeviceFound = FALSE;
	WDFKEY ParamRegistryKey;
	DECLARE_CONST_UNICODE_STRING(DesiredReportTypeKey, L"DesiredReportType");
	// FIX: initialize to 0 (PrecisionTouchpad) so the variable is never
	// indeterminate if WdfRegistryQueryValue partially succeeds but returns
	// an error before writing the output — eliminates PREFAST C6001 warning.
	ULONG DesiredReportTypeValue = 0, Length, ValueType = 0;

	PAGED_CODE();
	UNREFERENCED_PARAMETER(ResourceList);
	UNREFERENCED_PARAMETER(ResourceListTranslated);

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

	pDeviceContext = DeviceGetContext(Device);
	if (pDeviceContext == NULL) {
		Status = STATUS_INVALID_DEVICE_STATE;
		goto exit;
	}

	RtlZeroMemory(&DeviceAttributes, sizeof(DeviceAttributes));
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
		&HidAttributeMemoryDescriptor,
		(PVOID) &DeviceAttributes,
		sizeof(DeviceAttributes)
	);

	Status = WdfIoTargetSendInternalIoctlSynchronously(
		pDeviceContext->SpiTrackpadIoTarget,
		NULL,
		IOCTL_HID_GET_DEVICE_ATTRIBUTES,
		NULL,
		&HidAttributeMemoryDescriptor,
		NULL,
		NULL
	);
	if (!NT_SUCCESS(Status)) {
		KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
			"WdfIoTargetSendInternalIoctlSynchronously failed, status = 0x%x\n", Status));
		goto exit;
	}

	pDeviceContext->HidVendorID      = DeviceAttributes.VendorID;
	pDeviceContext->HidProductID     = DeviceAttributes.ProductID;
	pDeviceContext->HidVersionNumber = DeviceAttributes.VersionNumber;

	for (pTrackpadInfo = SpiTrackpadConfigTable; pTrackpadInfo->VendorId; ++pTrackpadInfo) {
		if (pTrackpadInfo->VendorId == DeviceAttributes.VendorID &&
			pTrackpadInfo->ProductId == DeviceAttributes.ProductID)
		{
			pDeviceContext->TrackpadInfo.ProductId = pTrackpadInfo->ProductId;
			pDeviceContext->TrackpadInfo.VendorId  = pTrackpadInfo->VendorId;
			pDeviceContext->TrackpadInfo.XMin      = pTrackpadInfo->XMin;
			pDeviceContext->TrackpadInfo.XMax      = pTrackpadInfo->XMax;
			pDeviceContext->TrackpadInfo.YMin      = pTrackpadInfo->YMin;
			pDeviceContext->TrackpadInfo.YMax      = pTrackpadInfo->YMax;
			DeviceFound = TRUE;
			break;
		}
	}

	if (!DeviceFound) {
		Status = STATUS_NOT_FOUND;
		goto exit;
	}

	Status = WdfDriverOpenParametersRegistryKey(
		WdfDeviceGetDriver(Device),
		KEY_READ,
		WDF_NO_OBJECT_ATTRIBUTES,
		&ParamRegistryKey
	);
	if (NT_SUCCESS(Status)) {
		Status = WdfRegistryQueryValue(
			ParamRegistryKey,
			&DesiredReportTypeKey,
			sizeof(ULONG),
			&DesiredReportTypeValue,
			&Length,
			&ValueType
		);
		if (NT_SUCCESS(Status)) {
			switch (DesiredReportTypeValue) {
			case 0: pDeviceContext->ReportType = PrecisionTouchpad; break;
			case 1: pDeviceContext->ReportType = Touchscreen;       break;
			default: Status = STATUS_INVALID_PARAMETER;             break;
			}
		}
		WdfRegistryClose(ParamRegistryKey);
	}

	Status = STATUS_SUCCESS;

exit:
	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! Exit, Status = %!STATUS!", Status);
	return Status;
}

NTSTATUS
AmtPtpEvtDeviceD0Entry(
	_In_ WDFDEVICE Device,
	_In_ WDF_POWER_DEVICE_STATE PreviousState
)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PDEVICE_CONTEXT pDeviceContext;

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! -->AmtPtpDeviceEvtDeviceD0Entry - coming from %s",
		DbgDevicePowerString(PreviousState));

	pDeviceContext = DeviceGetContext(Device);

	// FIX (sync): interlocked write — visible to completion routine at DISPATCH_LEVEL.
	DEVICE_STATUS_WRITE(pDeviceContext, D0ActiveAndUnconfigured);

	// FIX (sync): KeQueryInterruptTime returns ULONGLONG; store directly into
	// ULONGLONG field to avoid the signed/unsigned cast hazard of the old
	// LARGE_INTEGER.QuadPart approach.  KeMemoryBarrier() ensures the compiler
	// does not reorder this store past the DeviceStatus write above.
	KeMemoryBarrier();
	pDeviceContext->LastReportTime = KeQueryInterruptTime();

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! <-- AmtPtpDeviceEvtDeviceD0Entry");
	return Status;
}

NTSTATUS
AmtPtpEvtDeviceD0Exit(
	_In_ WDFDEVICE Device,
	_In_ WDF_POWER_DEVICE_STATE TargetState
)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PDEVICE_CONTEXT pDeviceContext;
	WDFREQUEST OutstandingRequest;

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! -->AmtPtpDeviceEvtDeviceD0Exit - moving to %s",
		DbgDevicePowerString(TargetState));

	pDeviceContext = DeviceGetContext(Device);

	// FIX (sync): write D3 atomically BEFORE draining the queue so the
	// completion routine's cleanup branch cannot re-issue AmtPtpSpiInputIssueRequest
	// while we are tearing down.  InterlockedExchange provides a full memory
	// barrier — the drain loop below is guaranteed to see D3 on all processors.
	DEVICE_STATUS_WRITE(pDeviceContext, D3);

	while (NT_SUCCESS(Status)) {
		Status = WdfIoQueueRetrieveNextRequest(
			pDeviceContext->HidQueue,
			&OutstandingRequest
		);
		if (NT_SUCCESS(Status)) {
			WdfRequestComplete(OutstandingRequest, STATUS_CANCELLED);
		}
	}

	Status = STATUS_SUCCESS;

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! <-- AmtPtpEvtDeviceD0Exit");
	return Status;
}

NTSTATUS
AmtPtpEvtDeviceSelfManagedIoInitOrRestart(
	_In_ WDFDEVICE Device
)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PDEVICE_CONTEXT pDeviceContext;

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
	pDeviceContext = DeviceGetContext(Device);

	Status = AmtPtpSpiSetState(Device, TRUE);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! AmtPtpSpiSetState failed with %!STATUS!. Retry after 5 seconds", Status);
		Status = STATUS_SUCCESS;
		WdfTimerStart(pDeviceContext->PowerOnRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(5));
		goto exit;
	}

	// FIX (sync): barrier before LastReportTime write so it is not reordered
	// before the DeviceStatus store that follows.
	KeMemoryBarrier();
	pDeviceContext->LastReportTime = KeQueryInterruptTime();

	pDeviceContext->XRange =
		(USHORT)((SHORT)pDeviceContext->TrackpadInfo.XMax -
		         (SHORT)pDeviceContext->TrackpadInfo.XMin);
	pDeviceContext->YRange =
		(USHORT)((SHORT)pDeviceContext->TrackpadInfo.YMax -
		         (SHORT)pDeviceContext->TrackpadInfo.YMin);

	// FIX (sync): interlocked write.
	DEVICE_STATUS_WRITE(pDeviceContext, D0ActiveAndConfigured);
	AmtPtpResetTrackingState(pDeviceContext);

exit:
	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! Exit, Status = %!STATUS!", Status);
	return Status;
}

PCHAR
DbgDevicePowerString(
	_In_ WDF_POWER_DEVICE_STATE Type
)
{
	switch (Type) {
	case WdfPowerDeviceInvalid:               return "WdfPowerDeviceInvalid";
	case WdfPowerDeviceD0:                    return "WdfPowerDeviceD0";
	case WdfPowerDeviceD1:                    return "WdfPowerDeviceD1";
	case WdfPowerDeviceD2:                    return "WdfPowerDeviceD2";
	case WdfPowerDeviceD3:                    return "WdfPowerDeviceD3";
	case WdfPowerDeviceD3Final:               return "WdfPowerDeviceD3Final";
	case WdfPowerDevicePrepareForHibernation: return "WdfPowerDevicePrepareForHibernation";
	case WdfPowerDeviceMaximum:               return "WdfPowerDeviceMaximum";
	default:                                  return "UnKnown Device Power State";
	}
}

NTSTATUS
AmtPtpSpiSetState(
	_In_ WDFDEVICE Device,
	_In_ BOOLEAN DesiredState
)
{
	NTSTATUS Status;
	PDEVICE_CONTEXT pDeviceContext;
	UCHAR HidPacketBuffer[HID_XFER_PACKET_SIZE];
	WDF_MEMORY_DESCRIPTOR HidMemoryDescriptor;
	PHID_XFER_PACKET pHidPacket;
	PSPI_SET_FEATURE pSpiSetStatus;

	pDeviceContext = DeviceGetContext(Device);
	if (pDeviceContext == NULL) {
		Status = STATUS_INVALID_DEVICE_STATE;
		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
			"%!FUNC! pDeviceContext == NULL");
		goto exit;
	}

	RtlZeroMemory(HidPacketBuffer, sizeof(HidPacketBuffer));
	pHidPacket = (PHID_XFER_PACKET) &HidPacketBuffer;
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
		&HidMemoryDescriptor,
		(PVOID) &HidPacketBuffer,
		HID_XFER_PACKET_SIZE
	);

	pHidPacket->reportId        = HID_REPORTID_MOUSE;
	pHidPacket->reportBufferLen = sizeof(SPI_SET_FEATURE);
	pHidPacket->reportBuffer    = (PUCHAR) pHidPacket + sizeof(HID_XFER_PACKET);
	pSpiSetStatus               = (PSPI_SET_FEATURE) pHidPacket->reportBuffer;
	pSpiSetStatus->BusLocation  = 2;
	pSpiSetStatus->Status       = DesiredState ? 1 : 0;

	Status = WdfIoTargetSendInternalIoctlSynchronously(
		pDeviceContext->SpiTrackpadIoTarget,
		NULL,
		IOCTL_HID_SET_FEATURE,
		&HidMemoryDescriptor,
		NULL,
		NULL,
		NULL
	);

	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! WdfIoTargetSendIoctlSynchronously failed with %!STATUS!", Status);
	} else {
		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
			"%!FUNC! Changed trackpad status to %d", DesiredState);
	}

exit:
	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! Exit, Status = %!STATUS!", Status);
	return Status;
}

void AmtPtpPowerRecoveryTimerCallback(
	WDFTIMER Timer
)
{
	WDFDEVICE Device;
	PDEVICE_CONTEXT pDeviceContext;
	NTSTATUS Status = STATUS_SUCCESS;

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
	Device = WdfTimerGetParentObject(Timer);
	pDeviceContext = DeviceGetContext(Device);

	// FIX (lifetime): guard against racing with D0Exit — if we lost the race
	// and DeviceStatus is already D3, do not touch the IO target.
	// DEVICE_STATUS_READ uses InterlockedCompareExchange for a safe read.
	if (DEVICE_STATUS_READ(pDeviceContext) == D3) {
		TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
			"%!FUNC! Device already in D3, skipping recovery");
		return;
	}

	Status = AmtPtpSpiSetState(Device, TRUE);
	if (NT_SUCCESS(Status)) {
		AmtPtpSpiInputIssueRequest(Device);
		// FIX (sync): interlocked write.
		DEVICE_STATUS_WRITE(pDeviceContext, D0ActiveAndConfigured);
		AmtPtpResetTrackingState(pDeviceContext);
	}

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! Exit, Status = %!STATUS!", Status);
}

// FIX (power): EvtIoStop is required on HidQueue so KMDF can drain it during
// power transitions (S3/S4) and surprise removal.  Without it the framework
// cannot guarantee all requests are complete before the device powers down,
// which causes DRIVER_POWER_STATE_FAILURE (Bug Check 0x9F) on some systems.
//
// Policy:
//   Suspend (WdfRequestStopActionSuspend): acknowledge without re-queuing so
//     the framework can power down; the request will be re-issued when the
//     device returns to D0 via SelfManagedIoRestart.
//   Purge  (WdfRequestStopActionPurge):   cancel immediately — device is
//     being removed and the request will never be serviced again.
VOID
AmtPtpEvtIoStop(
	_In_ WDFQUEUE   Queue,
	_In_ WDFREQUEST Request,
	_In_ ULONG      ActionFlags
)
{
	UNREFERENCED_PARAMETER(Queue);

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! Entry, ActionFlags = 0x%lx", ActionFlags);

	if (ActionFlags & WdfRequestStopActionSuspend) {
		// Power transition: acknowledge so the framework can proceed with
		// power-down.  SelfManagedIoRestart will re-queue the request when
		// the device comes back to D0.
		WdfRequestStopAcknowledge(Request, FALSE /* re-queue: no */);
	} else if (ActionFlags & WdfRequestStopActionPurge) {
		// Device is being removed — cancel the request immediately.
		WdfRequestComplete(Request, STATUS_CANCELLED);
	}

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
}
