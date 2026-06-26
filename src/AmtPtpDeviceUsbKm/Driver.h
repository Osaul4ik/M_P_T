#pragma once

// 1. Визначаємо версії Windows (вони вже передаються через командний рядок,
//    але для безпеки продублюємо тут, щоб бути впевненими)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10  // 0xA000010
#endif

// 2. Включаємо базові заголовки драйвера
#include <ntddk.h>   // визначає _KERNEL_MODE при /kernel
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <wdfusb.h>

// 3. ЯВНО включаємо wdm.h, щоб отримати прототип KeQueryInterruptTimeToPrecise
#include <wdm.h>     // <-- ЦЕ КЛЮЧОВИЙ РЯДОК

// 4. Ваші власні заголовки
#include "device.h"
#include "queue.h"
#include "trace.h"

// Для HID використовуйте hidport.h замість Hid.h
#include <hidport.h>

NTSYSAPI
ULONGLONG
NTAPI
KeQueryInterruptTimeToPrecise(
    _Out_opt_ PULONGLONG TimeSinceInterrupt
);

EXTERN_C_START

// WDFDRIVER Events

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AmtPtpDeviceUsbKmEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP AmtPtpDeviceUsbKmEvtDriverContextCleanup;

EXTERN_C_END