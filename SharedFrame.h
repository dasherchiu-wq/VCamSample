#pragma once

#include <windows.h>

inline constexpr wchar_t VCAM_SHARED_FRAME_PATH[] = L"C:\\Users\\Public\\VCamSampleFrame.bin";
inline constexpr DWORD VCAM_SHARED_FRAME_MAGIC = 0x4D414356; // "VCAM"
inline constexpr DWORD VCAM_SHARED_FRAME_VERSION = 1;
inline constexpr DWORD VCAM_SHARED_FRAME_WIDTH = 1280;
inline constexpr DWORD VCAM_SHARED_FRAME_HEIGHT = 960;
inline constexpr DWORD VCAM_SHARED_FRAME_STRIDE = VCAM_SHARED_FRAME_WIDTH * 4;

struct SharedFrame
{
	DWORD magic;
	DWORD version;
	DWORD width;
	DWORD height;
	DWORD stride;
	volatile LONG sequence;
	ULONGLONG capturedAtTick;
	BYTE pixels[VCAM_SHARED_FRAME_STRIDE * VCAM_SHARED_FRAME_HEIGHT];
};
