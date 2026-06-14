#pragma once

#include <hid/HidCommon.h>

#define AAPL_WELLSPRING_T2_PTP_FINGER_COLLECTION_1 \
	BEGIN_COLLECTION, 0x02, /* Begin Collection: Logical */ \
		/* Begin a byte */ \
		LOGICAL_MAXIMUM, 0x01, /* Logical Maximum: 1 */ \
		USAGE, 0x47, /* Usage: Confidence */ \
		USAGE, 0x42, /* Usage: Tip switch */ \
		REPORT_COUNT, 0x02, /* Report Count: 2 */ \
		REPORT_SIZE, 0x01, /* Report Size: 1 */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		REPORT_SIZE, 0x01, /* Report Size: 1 */ \
		REPORT_COUNT, 0x06, /* Report Count: 6 */ \
		INPUT, 0x03, /* Input: (Const, Var, Abs) */ \
		/* End of a byte */ \
		/* Begin of 4 bytes */ \
		REPORT_COUNT, 0x01, /* Report Count: 1 */ \
		REPORT_SIZE, 0x20, /* Report Size: 0x20 (4 bytes) -- Windows PTP spec: ContactID is 32-bit */ \
		LOGICAL_MAXIMUM_3, 0xff, 0xff, 0xff, 0x7f, /* Logical Maximum: 0x7FFFFFFF */ \
		USAGE, 0x51, /* Usage: Contact Identifier */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		/* End of 4 bytes */ \
		/* Begin of 4 bytes (X+Y) */ \
		USAGE_PAGE, 0x01, /* Usage Page: Generic Desktop */ \
		REPORT_SIZE, 0x10, /* Report Size: 0x10 (2 bytes) */ \
		REPORT_COUNT, 0x01, /* Report Count: 1 */ \
		LOGICAL_MAXIMUM_2, 0x20, 0x4e, /* Logical Maximum: 20000 */ \
		UNIT_EXPONENT, 0x0e, /* Unit exponent: -2 */ \
		UNIT, 0x11, /* Unit: SI Length (cm) */ \
		USAGE, 0x30, /* Usage: X */ \
		PHYSICAL_MAXIMUM_2, 0x14, 0x05, /* Physical Maximum: 1300 */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		PHYSICAL_MAXIMUM_2, 0x52, 0x03, /* Physical Maximum: 850 */ \
		LOGICAL_MAXIMUM_2, 0xe0, 0x2e, /* Logical Maximum: 12000 */ \
		USAGE, 0x31, /* Usage: Y */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		PHYSICAL_MAXIMUM, 0x00, /* Physical Maximum: 0 */ \
		UNIT_EXPONENT, 0x00, /* Unit exponent: 0 */ \
		UNIT, 0x00, /* Unit: None */ \
		/* End of 4 bytes */ \
	END_COLLECTION /* End Collection */ \

#define AAPL_WELLSPRING_T2_PTP_FINGER_COLLECTION_2 \
	BEGIN_COLLECTION, 0x02, /* Begin Collection: Logical */ \
		/* Begin a byte */ \
		LOGICAL_MAXIMUM, 0x01, /* Logical Maximum: 1 */ \
		USAGE, 0x47, /* Usage: Confidence */ \
		USAGE, 0x42, /* Usage: Tip switch */ \
		REPORT_COUNT, 0x02, /* Report Count: 2 */ \
		REPORT_SIZE, 0x01, /* Report Size: 1 */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		REPORT_SIZE, 0x01, /* Report Size: 1 */ \
		REPORT_COUNT, 0x06, /* Report Count: 6 */ \
		INPUT, 0x03, /* Input: (Const, Var, Abs) */ \
		/* End of a byte */ \
		/* Begin of 4 bytes */ \
		REPORT_COUNT, 0x01, /* Report Count: 1 */ \
		REPORT_SIZE, 0x20, /* Report Size: 0x20 (4 bytes) -- Windows PTP spec: ContactID is 32-bit */ \
		LOGICAL_MAXIMUM_3, 0xff, 0xff, 0xff, 0x7f, /* Logical Maximum: 0x7FFFFFFF */ \
		USAGE, 0x51, /* Usage: Contact Identifier */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		/* End of 4 bytes */ \
		/* Begin of 4 bytes (X+Y) */ \
		USAGE_PAGE, 0x01, /* Usage Page: Generic Desktop */ \
		REPORT_SIZE, 0x10, /* Report Size: 0x10 (2 bytes) */ \
		REPORT_COUNT, 0x01, /* Report Count: 1 */ \
		LOGICAL_MAXIMUM_2, 0x20, 0x4e, /* Logical Maximum: 20000 */ \
		UNIT_EXPONENT, 0x0e, /* Unit exponent: -2 */ \
		UNIT, 0x11, /* Unit: SI Length (cm) */ \
		USAGE, 0x30, /* Usage: X */ \
		PHYSICAL_MAXIMUM_2, 0x14, 0x05, /* Physical Maximum: 1045 */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		PHYSICAL_MAXIMUM_2, 0x52, 0x03, /* Physical Maximum: 750 */ \
		LOGICAL_MAXIMUM_2, 0xe0, 0x2e, /* Logical Maximum: 12000 */ \
		USAGE, 0x31, /* Usage: Y */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		/* End of 4 bytes */ \
	END_COLLECTION /* End Collection */ \

/*
 * Windows Precision Touchpad HID descriptor — T2 controller.
 *
 * Layout per the Windows PTP HID protocol specification:
 *   ReportID (1 byte)
 *   5 × Finger (9 bytes each = 45 bytes)
 *   ActualCount (1 byte)       — how many slots have meaningful data
 *   ScanTime   (2 bytes)
 *   ContactCount (1 byte)
 *   Button     (1 byte)
 *   Total: 51 bytes
 *
 * NOTE: The descriptor exposes 5 finger collections statically (required
 * by PTP spec), but the TotalContacts field is derived from
 * PTP_MAX_CONTACT_POINTS (5).  ActualCount tells the host that only
 * reportSlots of them carry valid data in any given report.
 */
#define AAPL_WELLSPRING_T2_PTP_TLC \
	USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
	USAGE, 0x05, /* Usage: Touch Pad */ \
	BEGIN_COLLECTION, 0x01, /* Begin Collection: Application */ \
		REPORT_ID, REPORTID_MULTITOUCH, /* Report ID: Multi-touch */ \
		/* --- 5 finger slots --- */ \
		USAGE, 0x22, /* Usage: Finger */ \
		AAPL_WELLSPRING_T2_PTP_FINGER_COLLECTION_1, /* Slot 0 */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		USAGE, 0x22, /* Usage: Finger */ \
		AAPL_WELLSPRING_T2_PTP_FINGER_COLLECTION_1, /* Slot 1 */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		USAGE, 0x22, /* Usage: Finger */ \
		AAPL_WELLSPRING_T2_PTP_FINGER_COLLECTION_2, /* Slot 2 */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		USAGE, 0x22, /* Usage: Finger */ \
		AAPL_WELLSPRING_T2_PTP_FINGER_COLLECTION_1, /* Slot 3 */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		USAGE, 0x22, /* Usage: Finger */ \
		AAPL_WELLSPRING_T2_PTP_FINGER_COLLECTION_2, /* Slot 4 */ \
		/* --- ActualCount --- */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		REPORT_SIZE, 0x08, /* Report Size: 8 bits */ \
		REPORT_COUNT, 0x01, /* Report Count: 1 */ \
		LOGICAL_MINIMUM, 0x00, \
		LOGICAL_MAXIMUM, PTP_MAX_CONTACT_POINTS, \
		USAGE, 0x57, /* Usage: Actual Count */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		/* --- ScanTime --- */ \
		REPORT_SIZE, 0x10, /* Report Size: 16 bits */ \
		REPORT_COUNT, 0x01, \
		UNIT_EXPONENT, 0x0c, /* Unit exponent: -4 */ \
		UNIT_2, 0x01, 0x10, /* Time: Second */ \
		PHYSICAL_MAXIMUM_2, 0xFF, 0x7F, \
		LOGICAL_MAXIMUM_2, 0xFF, 0x7F, \
		USAGE, 0x56, /* Usage: Scan Time */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		/* --- ContactCount (total contacts in frame) --- */ \
		REPORT_SIZE, 0x08, \
		REPORT_COUNT, 0x01, \
		USAGE, 0x54, /* Usage: Contact Count */ \
		LOGICAL_MINIMUM, 0x00, \
		LOGICAL_MAXIMUM, PTP_MAX_CONTACT_POINTS, \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		/* --- Button --- */ \
		USAGE_PAGE, 0x09, /* Usage Page: Button */ \
		USAGE, 0x01, /* Button 1 */ \
		LOGICAL_MINIMUM, 0x00, \
		LOGICAL_MAXIMUM, 0x01, \
		REPORT_SIZE, 0x01, \
		REPORT_COUNT, 0x01, \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		REPORT_COUNT, 0x07, \
		INPUT, 0x03, /* Input: (Const, Var, Abs) */ \
		/* --- Feature reports --- */ \
		USAGE_PAGE, 0x0d, \
		REPORT_ID, REPORTID_DEVICE_CAPS, \
		USAGE, 0x55, /* Usage: Maximum Contact Points */ \
		USAGE, 0x59, /* Usage: Touchpad Button Type */ \
		LOGICAL_MINIMUM, 0x00, \
		LOGICAL_MAXIMUM_2, 0xff, 0x00, \
		REPORT_SIZE, 0x08, \
		REPORT_COUNT, 0x02, \
		FEATURE, 0x02, \
		USAGE_PAGE_1, 0x00, 0xff, \
		REPORT_ID, REPORTID_PTPHQA, \
		USAGE, 0xc5, \
		LOGICAL_MINIMUM, 0x00, \
		LOGICAL_MAXIMUM_2, 0xff, 0x00, \
		REPORT_SIZE, 0x08, \
		REPORT_COUNT_2, 0x00, 0x01, \
		FEATURE, 0x02, \
	END_COLLECTION /* End Collection */