/*++

Module Name:

    device.c - Device handling events for example driver.

Abstract:

   This file contains the device entry points and callbacks.
    
Environment:

    Kernel-mode Driver Framework

--*/

#include "driver.h"
#include "device.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmCreateDevice)
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmEvtDevicePrepareHardware)
#endif

_IRQL_requires_(PASSIVE_LEVEL)
static const struct BCM5974_CONFIG*
AmtPtpGetDeviceConfig(
	_In_ USB_DEVICE_DESCRIPTOR deviceInfo
)
{
	USHORT id = deviceInfo.idProduct;
	const struct BCM5974_CONFIG* cfg;

	for (cfg = Bcm5974ConfigTable; cfg->identification; ++cfg) {
		if (cfg->identification == id) {
			return cfg;
		}
	}

	// Generic fallback
	TraceEvents(
		TRACE_LEVEL_WARNING,
		TRACE_DRIVER,
		"%!FUNC! Selected a generic fallback configuration"
	);

	return &Bcm5974ConfigTable[0];
}

NTSTATUS
AmtPtpDeviceUsbKmCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES   deviceAttributes;
    PDEVICE_CONTEXT deviceContext;
    WDFDEVICE device;
    NTSTATUS status;

    PAGED_CODE();

	// Initialize power callback (prep, D0 entry & exit)
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = AmtPtpDeviceUsbKmEvtDevicePrepareHardware;
	pnpPowerCallbacks.EvtDeviceD0Entry = AmtPtpEvtDeviceD0Entry;
	pnpPowerCallbacks.EvtDeviceD0Exit = AmtPtpEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);

    if (NT_SUCCESS(status)) {
        deviceContext = DeviceGetContext(device);

        // Initialize PTP reporting defaults.
        deviceContext->PtpReportButton = TRUE;
        deviceContext->PtpReportTouch  = TRUE;

        status = WdfDeviceCreateDeviceInterface(
            device,
            &GUID_DEVINTERFACE_AmtPtpDeviceUsbKm,
            NULL
            );

        if (NT_SUCCESS(status)) {
            status = AmtPtpDeviceUsbKmQueueInitialize(device);
        }
    }

    return status;
}

// Helper: reset all slot tracking state to idle.
static VOID
AmtPtpResetSlotState(
    _In_ PDEVICE_CONTEXT pDeviceContext
)
{
    RtlZeroMemory(pDeviceContext->SlotInUse,          sizeof(pDeviceContext->SlotInUse));
    RtlZeroMemory(pDeviceContext->SlotPendingRelease, sizeof(pDeviceContext->SlotPendingRelease));
    RtlZeroMemory(pDeviceContext->SlotCooldown,       sizeof(pDeviceContext->SlotCooldown));
    RtlZeroMemory(pDeviceContext->SlotTipConfirmed,   sizeof(pDeviceContext->SlotTipConfirmed));
    RtlZeroMemory(pDeviceContext->SlotFingerKey,      sizeof(pDeviceContext->SlotFingerKey));
    RtlZeroMemory(pDeviceContext->LastNormX,          sizeof(pDeviceContext->LastNormX));
    RtlZeroMemory(pDeviceContext->LastNormY,          sizeof(pDeviceContext->LastNormY));
    RtlZeroMemory(pDeviceContext->HystX,              sizeof(pDeviceContext->HystX));
    RtlZeroMemory(pDeviceContext->HystY,              sizeof(pDeviceContext->HystY));
}

NTSTATUS
AmtPtpDeviceUsbKmEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourceList,
    _In_ WDFCMRESLIST ResourceListTranslated
    )
{
    NTSTATUS status;
    PDEVICE_CONTEXT pDeviceContext;
	WDF_USB_DEVICE_INFORMATION deviceInfo;
	ULONG waitWakeEnable = FALSE;

    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(ResourceListTranslated);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    status = STATUS_SUCCESS;
    pDeviceContext = DeviceGetContext(Device);

    if (pDeviceContext->UsbDevice == NULL) {
		status = WdfUsbTargetDeviceCreate(Device,
			WDF_NO_OBJECT_ATTRIBUTES,
			&pDeviceContext->UsbDevice
		);

        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
				"WdfUsbTargetDeviceCreate failed 0x%x", status);
            return status;
        }
    }

	WdfUsbTargetDeviceGetDeviceDescriptor(
		pDeviceContext->UsbDevice,
		&pDeviceContext->DeviceDescriptor
	);

	pDeviceContext->DeviceInfo = AmtPtpGetDeviceConfig(pDeviceContext->DeviceDescriptor);
	if (pDeviceContext->DeviceInfo == NULL) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
			"AmtPtpGetDeviceConfig failed to find the device config");
		status = STATUS_INVALID_DEVICE_STATE;
		return status;
	}

	WDF_USB_DEVICE_INFORMATION_INIT(&deviceInfo);
	status = WdfUsbTargetDeviceRetrieveInformation(
		pDeviceContext->UsbDevice,
		&deviceInfo
	);

	if (NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
			"%!FUNC! IsDeviceHighSpeed: %s",
			(deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_AT_HIGH_SPEED) ? "TRUE" : "FALSE");
		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
			"%!FUNC! IsDeviceSelfPowered: %s",
			(deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_SELF_POWERED) ? "TRUE" : "FALSE");

		waitWakeEnable = deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_REMOTE_WAKE_CAPABLE;

		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
			"%!FUNC! IsDeviceRemoteWakeable: %s",
			waitWakeEnable ? "TRUE" : "FALSE");

		pDeviceContext->UsbDeviceTraits = deviceInfo.Traits;
	} else {
		pDeviceContext->UsbDeviceTraits = 0;
	}

	status = SelectInterruptInterface(Device);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, 
			"%!FUNC! SelectInterruptInterface failed with %!STATUS!", status);
		return status;
	}

	status = AmtPtpConfigContReaderForInterruptEndPoint(pDeviceContext);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, 
			"%!FUNC! AmtPtpConfigContReaderForInterruptEndPoint failed with %!STATUS!", status);
		return status;
	}

	// PTP reporting defaults.
	pDeviceContext->PtpReportButton = TRUE;
	pDeviceContext->PtpReportTouch  = TRUE;

	// Reset all slot tracking state.
	AmtPtpResetSlotState(pDeviceContext);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
    return status;
}

// D0 Entry
NTSTATUS
AmtPtpEvtDeviceD0Entry(
	_In_ WDFDEVICE Device,
	_In_ WDF_POWER_DEVICE_STATE PreviousState
)
{
	PDEVICE_CONTEXT         pDeviceContext;
	NTSTATUS                status;
	BOOLEAN                 isTargetStarted;

	pDeviceContext = DeviceGetContext(Device);
	isTargetStarted = FALSE;

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! -->AmtPtpDeviceEvtDeviceD0Entry - coming from %s",
		DbgDevicePowerString(PreviousState));

	if (pDeviceContext->PtpReportButton || pDeviceContext->IsWellspringModeOn) {
		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
			"%!FUNC! Start Wellspring Mode");

		status = AmtPtpSetWellspringMode(pDeviceContext, TRUE);
		if (!NT_SUCCESS(status)) {
			TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
				"%!FUNC! Start Wellspring Mode failed with %!STATUS!", status);
		}
	}

	// Re-entering D0: clear all slot state so stale contacts are not re-reported.
	AmtPtpResetSlotState(pDeviceContext);

	KeQueryPerformanceCounter(&pDeviceContext->LastReportTime);

	status = WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe));
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! Failed to start interrupt pipe %!STATUS!", status);
		goto end;
	}

	isTargetStarted = TRUE;

end:
	if (!NT_SUCCESS(status)) {
		if (isTargetStarted) {
			WdfIoTargetStop(
				WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe),
				WdfIoTargetCancelSentIo);
		}
	}

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! <--AmtPtpDeviceEvtDeviceD0Entry");
	return status;
}

// D0 Exit
NTSTATUS
AmtPtpEvtDeviceD0Exit(
	_In_ WDFDEVICE Device,
	_In_ WDF_POWER_DEVICE_STATE TargetState
)
{
	PDEVICE_CONTEXT pDeviceContext;
	NTSTATUS        status;

	PAGED_CODE();
	status = STATUS_SUCCESS;

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! -->AmtPtpDeviceEvtDeviceD0Exit - moving to %s",
		DbgDevicePowerString(TargetState));

	pDeviceContext = DeviceGetContext(Device);

	if (pDeviceContext->InterruptPipe == NULL) {
		TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
			"%!FUNC! InterruptPipe is NULL, skipping pipe stop");
	} else {
		WdfIoTargetStop(
			WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe),
			WdfIoTargetCancelSentIo);
	}

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! Cancel Wellspring Mode");

	status = AmtPtpSetWellspringMode(pDeviceContext, FALSE);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
			"%!FUNC! Cancel Wellspring Mode failed with %!STATUS!. Continuing.", status);
		status = STATUS_SUCCESS;
	}

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! <--AmtPtpDeviceEvtDeviceD0Exit");
	return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
SelectInterruptInterface(
	_In_ WDFDEVICE Device
)
{
	WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
	NTSTATUS                            status = STATUS_SUCCESS;
	PDEVICE_CONTEXT                     pDeviceContext;
	WDFUSBPIPE                          pipe;
	WDF_USB_PIPE_INFORMATION            pipeInfo;
	UCHAR                               index;
	UCHAR                               numberConfiguredPipes;

	PAGED_CODE();

	pDeviceContext = DeviceGetContext(Device);
	WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&configParams);

	status = WdfUsbTargetDeviceSelectConfig(pDeviceContext->UsbDevice,
		WDF_NO_OBJECT_ATTRIBUTES, &configParams);

	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
			"WdfUsbTargetDeviceSelectConfig failed %!STATUS!", status);
		return status;
	}

	pDeviceContext->UsbInterface = configParams.Types.SingleInterface.ConfiguredUsbInterface;
	numberConfiguredPipes = configParams.Types.SingleInterface.NumberConfiguredPipes;

	for (index = 0; index < numberConfiguredPipes; index++) {
		WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);

		pipe = WdfUsbInterfaceGetConfiguredPipe(
			pDeviceContext->UsbInterface, index, &pipeInfo);

		WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
			"%!FUNC! Found USB pipe type %d", pipeInfo.PipeType);

		if (WdfUsbPipeTypeInterrupt == pipeInfo.PipeType) {
			pDeviceContext->InterruptPipe = pipe;
			break;
		}
	}

	if (!pDeviceContext->InterruptPipe) {
		status = STATUS_INVALID_DEVICE_STATE;
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
			"%!FUNC! Device is not configured properly %!STATUS!", status);
	}

	return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpSetWellspringMode(
	_In_ PDEVICE_CONTEXT DeviceContext,
	_In_ BOOLEAN IsWellspringModeOn
)
{
	NTSTATUS					status;
	WDF_USB_CONTROL_SETUP_PACKET	setupPacket;
	WDF_MEMORY_DESCRIPTOR		memoryDescriptor;
	ULONG						cbTransferred;
	WDFMEMORY					bufHandle = NULL;
	unsigned char*				buffer;

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

	// TYPE3 devices need no mode switch — just record the desired state.
	if (DeviceContext->DeviceInfo->tp_type == TYPE3) {
		DeviceContext->IsWellspringModeOn = IsWellspringModeOn;
		return STATUS_SUCCESS;
	}

	status = WdfMemoryCreate(
		WDF_NO_OBJECT_ATTRIBUTES,
		PagedPool,
		POOL_TAG_PTP_CONTROL,
		DeviceContext->DeviceInfo->um_size,
		&bufHandle,
		&buffer
	);

	if (!NT_SUCCESS(status)) {
		goto cleanup;
	}

	RtlZeroMemory(buffer, DeviceContext->DeviceInfo->um_size);

	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
		&memoryDescriptor,
		buffer,
		sizeof(DeviceContext->DeviceInfo->um_size)
	);

	WDF_USB_CONTROL_SETUP_PACKET_INIT(
		&setupPacket,
		BmRequestDeviceToHost,
		BmRequestToInterface,
		BCM5974_WELLSPRING_MODE_READ_REQUEST_ID,
		(USHORT)DeviceContext->DeviceInfo->um_req_val,
		(USHORT)DeviceContext->DeviceInfo->um_req_idx
	);
	setupPacket.Packet.bm.Request.Type = BmRequestClass;

	status = WdfUsbTargetDeviceSendControlTransferSynchronously(
		DeviceContext->UsbDevice, WDF_NO_HANDLE, NULL,
		&setupPacket, &memoryDescriptor, &cbTransferred
	);

	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
			"%!FUNC! Control read failed %!STATUS!, cbTransferred=%llu um_size=%d",
			status, cbTransferred, DeviceContext->DeviceInfo->um_size);
		goto cleanup;
	}

	buffer[DeviceContext->DeviceInfo->um_switch_idx] = IsWellspringModeOn ?
		(unsigned char)DeviceContext->DeviceInfo->um_switch_on :
		(unsigned char)DeviceContext->DeviceInfo->um_switch_off;

	WDF_USB_CONTROL_SETUP_PACKET_INIT(
		&setupPacket,
		BmRequestHostToDevice,
		BmRequestToInterface,
		BCM5974_WELLSPRING_MODE_WRITE_REQUEST_ID,
		(USHORT)DeviceContext->DeviceInfo->um_req_val,
		(USHORT)DeviceContext->DeviceInfo->um_req_idx
	);
	setupPacket.Packet.bm.Request.Type = BmRequestClass;

	status = WdfUsbTargetDeviceSendControlTransferSynchronously(
		DeviceContext->UsbDevice, WDF_NO_HANDLE, NULL,
		&setupPacket, &memoryDescriptor, &cbTransferred
	);

	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
			"%!FUNC! Control write failed %!STATUS!", status);
		goto cleanup;
	}

	DeviceContext->IsWellspringModeOn = IsWellspringModeOn;

cleanup:
	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
	if (bufHandle != NULL) {
		WdfObjectDelete(bufHandle);
		bufHandle = NULL;
	}
	return status;
}