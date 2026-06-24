// Queue definitions. Kernel-mode Driver Framework

EXTERN_C_START

// Per-queue context.
typedef struct _QUEUE_CONTEXT {
    ULONG PrivateDeviceData;  // placeholder
} QUEUE_CONTEXT, *PQUEUE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(QUEUE_CONTEXT, QueueGetContext)

NTSTATUS
AmtPtpDeviceUsbKmQueueInitialize(
    _In_ WDFDEVICE Device
    );

// IoQueue events
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL AmtPtpDeviceUsbKmEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_STOP AmtPtpDeviceUsbKmEvtIoStop;

EXTERN_C_END