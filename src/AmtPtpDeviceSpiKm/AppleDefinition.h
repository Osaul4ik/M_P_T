#pragma once

#define SPI_TRACKPAD_MAX_FINGERS 10
#define DEVICE_VID 0x8910

typedef struct _SPI_TRACKPAD_FINGER
{
	SHORT OriginalX;
	SHORT OriginalY;
	SHORT X;
	SHORT Y;
	SHORT HorizontalAccel;
	SHORT VerticalAccel;
	SHORT ToolMajor;
	SHORT ToolMinor;
	SHORT Orientation;
	SHORT TouchMajor;
	SHORT TouchMinor;
	SHORT Rsvd1;
	SHORT Rsvd2;
	SHORT Pressure;
	SHORT Rsvd3;
} SPI_TRACKPAD_FINGER, *PSPI_TRACKPAD_FINGER;

typedef struct _SPI_TRACKPAD_PACKET
{
	UINT8 PacketType;
	UINT8 ClickOccurred;
	UINT8 Reserved0[5];
	UINT8 IsFinger;
	UINT8 Reserved1[16];
	UINT8 FingerDataLength;
	UINT8 Reserved2[5];
	UINT8 NumOfFingers;
	UINT8 ClickOccurred2;
	UINT8 State1;
	UINT8 State2;
	UINT8 State3;
	UINT8 Padding;
	UINT8 Reserved3[10];
	SPI_TRACKPAD_FINGER Fingers[SPI_TRACKPAD_MAX_FINGERS];
} SPI_TRACKPAD_PACKET, *PSPI_TRACKPAD_PACKET;

typedef struct _SPI_SET_FEATURE {
	UINT8 BusLocation;
	UINT8 Status;
} SPI_SET_FEATURE, *PSPI_SET_FEATURE;

#define HID_REPORTID_MOUSE  2
#define HID_XFER_PACKET_SIZE 255

static const SPI_TRACKPAD_INFO SpiTrackpadConfigTable[] = 
{
	/* MacBookPro11,1 / MacBookPro12,1 */
	{ 0x05ac, 0x0272, -4750, 5280, -150, 6730 },
	{ 0x05ac, 0x0273, -4750, 5280, -150, 6730 },
	/* MacBook9 */
	{ 0x05ac, 0x0275, -5087, 5579, -128, 6089 },
	/* MacBookPro14,1 / MacBookPro14,2 */
	{ 0x05ac, 0x0276, -6243, 6749, -170, 7685 },
	{ 0x05ac, 0x0277, -6243, 6749, -170, 7685 },
	/* MacBookPro14,3 */
	{ 0x05ac, 0x0278, -7456, 7976, -163, 9283 },
	/* MacBook10 */
	{ 0x05ac, 0x0279, -5087, 5579, -128, 6089 },
	/* MacBookPro15,1 / MacBookPro15,3 (2018, 15-inch T2) */
	{ 0x05ac, 0x027a, -7456, 7976, -163, 9283 },
	{ 0x05ac, 0x027b, -7456, 7976, -163, 9283 },
	/* MacBookPro15,2 (2018, 13-inch T2) */
	{ 0x05ac, 0x027c, -6243, 6749, -170, 7685 },
	{ 0x05ac, 0x027d, -6243, 6749, -170, 7685 },
	/* MacBookPro16,1 / MacBookPro16,4 (2019, 16-inch T2) - YOUR DEVICE */
	{ 0x05ac, 0x0280, -7456, 7976, -163, 9283 },
	/* MacBookPro16,2 / MacBookPro16,3 (2020, 13-inch T2) */
	{ 0x05ac, 0x0281, -6243, 6749, -170, 7685 },
	{ 0x05ac, 0x0282, -6243, 6749, -170, 7685 },
	/* MacBookAir7,2 fallback */
	{ 0x05ac, 0x0290, -5087, 5579, -128, 6089 },
	{ 0x05ac, 0x0291, -5087, 5579, -128, 6089 },
	/* Terminator */
	{ 0 }
};
