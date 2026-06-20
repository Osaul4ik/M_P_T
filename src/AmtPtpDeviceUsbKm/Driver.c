// Driver entry points and callbacks. Kernel-mode Driver Framework

#define INITGUID
#include <initguid.h>
#include "driver.h"
#include "driver.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmEvtDeviceAdd)
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmEvtDriverContextCleanup)
#endif


NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
// DriverEntry — called after driver load. Initialises WPP tracing and
// registers EvtDevice callback.
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;

    // Initialize WPP Tracing
    WPP_INIT_TRACING( DriverObject, RegistryPath );

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    // Register cleanup callback for WPP_CLEANUP on driver unload.
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = AmtPtpDeviceUsbKmEvtDriverContextCleanup;

    WDF_DRIVER_CONFIG_INIT(&config,
                           AmtPtpDeviceUsbKmEvtDeviceAdd
                           );

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             &attributes,
                             &config,
                             WDF_NO_HANDLE
                             );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "WdfDriverCreate failed %!STATUS!", status);
        WPP_CLEANUP(DriverObject);
        return status;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");

    return status;
}

NTSTATUS
AmtPtpDeviceUsbKmEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
// EvtDeviceAdd — called by framework on AddDevice from PnP manager.
// Creates and initialises a device object.
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

	TraceEvents(
		TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"%!FUNC! Set FDO driver filter"
	);

	WdfFdoInitSetFilter(DeviceInit);
	WdfPdoInitAllowForwardingRequestToParent(DeviceInit);

    status = AmtPtpDeviceUsbKmCreateDevice(DeviceInit);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");

    return status;
}

VOID
AmtPtpDeviceUsbKmEvtDriverContextCleanup(
    _In_ WDFOBJECT DriverObject
    )
// EvtDriverContextCleanup — frees resources allocated in DriverEntry.
{
    UNREFERENCED_PARAMETER(DriverObject);

    PAGED_CODE ();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    // Stop WPP Tracing
    WPP_CLEANUP( WdfDriverWdmGetDriverObject( (WDFDRIVER) DriverObject) );

}