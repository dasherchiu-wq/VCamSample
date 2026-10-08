#include "pch.h"
#include "Undocumented.h"
#include "Tools.h"
#include "EnumNames.h"
#include "MFTools.h"
#include "FrameGenerator.h"
#include "MediaStream.h"
#include "MediaSource.h"
#include "../SharedFrame.h"

HRESULT MediaStream::Initialize(IMFMediaSource* source, int index)
{
	RETURN_HR_IF_NULL(E_POINTER, source);
	_source = source;
	_index = index;

	RETURN_IF_FAILED(SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_STREAM_ID, index));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes::MFFrameSourceTypes_Color));

	RETURN_IF_FAILED(MFCreateEventQueue(&_queue));

	// The OBS capture path writes RGB32 pixels. Advertising NV12 lets clients select
	// a layout that cannot be filled with those pixels directly.
	auto types = wil::make_unique_cotaskmem_array<wil::com_ptr_nothrow<IMFMediaType>>(1);

#define NUM_IMAGE_COLS 1280 // 640
#define NUM_IMAGE_ROWS 960 //480

	wil::com_ptr_nothrow<IMFMediaType> rgbType;
	RETURN_IF_FAILED(MFCreateMediaType(&rgbType));
	rgbType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	rgbType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
	MFSetAttributeSize(rgbType.get(), MF_MT_FRAME_SIZE, NUM_IMAGE_COLS, NUM_IMAGE_ROWS);
	rgbType->SetUINT32(MF_MT_DEFAULT_STRIDE, NUM_IMAGE_COLS * 4);
	rgbType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	rgbType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
	MFSetAttributeRatio(rgbType.get(), MF_MT_FRAME_RATE, 30, 1);
	auto bitrate = (uint32_t)(NUM_IMAGE_COLS * NUM_IMAGE_ROWS * 4 * 8 * 30);
	rgbType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
	MFSetAttributeRatio(rgbType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	types[0] = rgbType.detach();

	if (types.size() > 1)
	{
		wil::com_ptr_nothrow<IMFMediaType> nv12Type;
		RETURN_IF_FAILED(MFCreateMediaType(&nv12Type));
		nv12Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		nv12Type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
		nv12Type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		nv12Type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
		MFSetAttributeSize(nv12Type.get(), MF_MT_FRAME_SIZE, NUM_IMAGE_COLS, NUM_IMAGE_ROWS);
		nv12Type->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT)(NUM_IMAGE_COLS * 1.5));
		MFSetAttributeRatio(nv12Type.get(), MF_MT_FRAME_RATE, 30, 1);
		// frame size * pixel bit size * framerate
		bitrate = (uint32_t)(NUM_IMAGE_COLS * 1.5 * NUM_IMAGE_ROWS * 8 * 30);
		nv12Type->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
		MFSetAttributeRatio(nv12Type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		types[1] = nv12Type.detach();
	}

	RETURN_IF_FAILED_MSG(MFCreateStreamDescriptor(_index, (DWORD)types.size(), types.get(), &_descriptor), "MFCreateStreamDescriptor failed");

	wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
	RETURN_IF_FAILED(_descriptor->GetMediaTypeHandler(&handler));
	TraceMFAttributes(handler.get(), L"MediaTypeHandler");
	RETURN_IF_FAILED(handler->SetCurrentMediaType(types[0]));

	return S_OK;
}

HRESULT MediaStream::Start(IMFMediaType* type)
{
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_allocator);

	if (type)
	{
		RETURN_IF_FAILED(type->GetGUID(MF_MT_SUBTYPE, &_format));
		WINTRACE(L"MediaStream::Start format: %s", GUID_ToStringW(_format).c_str());
	}

	// at this point, set D3D manager may have not been called
	// so we want to create a D2D1 renter target anyway
	RETURN_IF_FAILED(_generator.EnsureRenderTarget(NUM_IMAGE_COLS, NUM_IMAGE_ROWS));

	RETURN_IF_FAILED(_allocator->InitializeSampleAllocator(10, type));
	RETURN_IF_FAILED(_queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
	_state = MF_STREAM_STATE_RUNNING;
	return S_OK;
}

HRESULT MediaStream::Stop()
{
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_allocator);

	RETURN_IF_FAILED(_allocator->UninitializeSampleAllocator());
	RETURN_IF_FAILED(_queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr));
	_state = MF_STREAM_STATE_STOPPED;
	return S_OK;
}

MFSampleAllocatorUsage MediaStream::GetAllocatorUsage()
{
	return MFSampleAllocatorUsage_UsesProvidedAllocator;
}

HRESULT MediaStream::SetAllocator(IUnknown* allocator)
{
	RETURN_HR_IF_NULL(E_POINTER, allocator);
	_allocator.reset();
	RETURN_HR(allocator->QueryInterface(&_allocator));
}

HRESULT MediaStream::SetD3DManager(IUnknown* manager)
{
	RETURN_HR_IF_NULL(E_POINTER, manager);

	// Keep samples in system memory. A DXGI-backed IMFMediaBuffer cannot be
	// overwritten through IMFMediaBuffer::Lock, which left the generated test
	// pattern visible after the OBS window had been captured successfully.
	return S_OK;
}

void MediaStream::Shutdown()
{
	if (_queue)
	{
		LOG_IF_FAILED_MSG(_queue->Shutdown(), "Queue shutdown failed");
		_queue.reset();
	}

	_descriptor.reset();
	_source.reset();
	_attributes.reset();
	if (_sharedFrame)
	{
		UnmapViewOfFile(_sharedFrame);
		_sharedFrame = nullptr;
	}
	if (_sharedFrameMapping)
	{
		CloseHandle(_sharedFrameMapping);
		_sharedFrameMapping = nullptr;
	}
}

// IMFMediaEventGenerator
STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
	//WINTRACE(L"MediaSource::BeginGetEvent");
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->BeginGetEvent(pCallback, punkState));
	return S_OK;
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
	//WINTRACE(L"MediaStream::EndGetEvent");
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->EndGetEvent(pResult, ppEvent));
	return S_OK;
}

STDMETHODIMP MediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
	WINTRACE(L"MediaStream::GetEvent");
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->GetEvent(dwFlags, ppEvent));
	return S_OK;
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
	WINTRACE(L"MediaStream::QueueEvent");
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue));
	return S_OK;
}

// IMFMediaStream
STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** ppMediaSource)
{
	WINTRACE(L"MediaSource::GetMediaSource");
	RETURN_HR_IF_NULL(E_POINTER, ppMediaSource);
	*ppMediaSource = nullptr;
	RETURN_HR_IF(MF_E_SHUTDOWN, !_source);

	RETURN_IF_FAILED(_source.copy_to(ppMediaSource));
	return S_OK;
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor)
{
	WINTRACE(L"MediaStream::GetStreamDescriptor");
	RETURN_HR_IF_NULL(E_POINTER, ppStreamDescriptor);
	*ppStreamDescriptor = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_descriptor);

	RETURN_IF_FAILED(_descriptor.copy_to(ppStreamDescriptor));
	return S_OK;
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* pToken)
{
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_allocator || !_queue);

	wil::com_ptr_nothrow<IMFSample> sample;
	RETURN_IF_FAILED(_allocator->AllocateSample(&sample));
	RETURN_IF_FAILED(sample->SetSampleTime(MFGetSystemTime()));
	RETURN_IF_FAILED(sample->SetSampleDuration(333333));

	wil::com_ptr_nothrow<IMFSample> outSample;
	RETURN_IF_FAILED(_generator.Generate(sample.get(), _format, &outSample));

	// --- OBS CAPTURE ENGINE HOOK ---
	wil::com_ptr_nothrow<IMFMediaBuffer> mediaBuffer;
	if (SUCCEEDED(outSample->GetBufferByIndex(0, &mediaBuffer)))
	{
		wil::com_ptr_nothrow<IMF2DBuffer2> buffer2D;
		if (SUCCEEDED(mediaBuffer->QueryInterface(IID_PPV_ARGS(&buffer2D))))
		{
			BYTE* scanline = nullptr;
			BYTE* bufferStart = nullptr;
			LONG pitch = 0;
			DWORD bufferLength = 0;
			if (SUCCEEDED(buffer2D->Lock2DSize(MF2DBuffer_LockFlags_Write,
				&scanline, &pitch, &bufferStart, &bufferLength)))
			{
				// Prefer the Preview and Program projectors. OBS source projectors use
				// the same stable prefix, so accept one when those exact titles are absent.
				HWND obsWindow = FindWindowW(nullptr, L"Projector - Preview");
				if (!obsWindow)
					obsWindow = FindWindowW(nullptr, L"Projector - Program");

				if (!obsWindow)
				{
					struct ObsProjectorSearch
					{
						HWND window = nullptr;
					};
					ObsProjectorSearch search;
					EnumWindows([](HWND hwnd, LPARAM context) -> BOOL
						{
							wchar_t title[256]{};
							if (IsWindowVisible(hwnd) && GetWindowTextW(hwnd, title, _countof(title)) > 0 &&
								wcsncmp(title, L"Projector - ", 12) == 0)
							{
								reinterpret_cast<ObsProjectorSearch*>(context)->window = hwnd;
								return FALSE;
							}
							return TRUE;
						}, reinterpret_cast<LPARAM>(&search));
					obsWindow = search.window;
				}

				if (!obsWindow)
					obsWindow = FindWindowW(nullptr, L"Windowed Projector (Program)");

				if (obsWindow)
				{
					RECT clientRect{};
					if (GetClientRect(obsWindow, &clientRect) && clientRect.right > 0 && clientRect.bottom > 0)
					{
						HDC hdcWindow = GetDC(obsWindow);
						HDC hdcMem = hdcWindow ? CreateCompatibleDC(hdcWindow) : nullptr;

						BITMAPINFO bitmapInfo{};
						bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
						bitmapInfo.bmiHeader.biWidth = NUM_IMAGE_COLS;
						bitmapInfo.bmiHeader.biHeight = -NUM_IMAGE_ROWS;
						bitmapInfo.bmiHeader.biPlanes = 1;
						bitmapInfo.bmiHeader.biBitCount = 32;
						bitmapInfo.bmiHeader.biCompression = BI_RGB;

						void* dibPixels = nullptr;
						HBITMAP hBitmap = hdcMem ? CreateDIBSection(hdcWindow, &bitmapInfo,
							DIB_RGB_COLORS, &dibPixels, nullptr, 0) : nullptr;
						HGDIOBJ hOld = hBitmap ? SelectObject(hdcMem, hBitmap) : nullptr;

						if (hOld && dibPixels)
						{
							SetStretchBltMode(hdcMem, HALFTONE);
							SetBrushOrgEx(hdcMem, 0, 0, nullptr);
							if (StretchBlt(hdcMem, 0, 0, NUM_IMAGE_COLS, NUM_IMAGE_ROWS,
								hdcWindow, 0, 0, clientRect.right, clientRect.bottom, SRCCOPY))
							{
								const DWORD rowBytes = NUM_IMAGE_COLS * 4;
								const DWORD absolutePitch = static_cast<DWORD>(pitch < 0 ? -pitch : pitch);
								if (scanline && absolutePitch >= rowBytes &&
									bufferLength >= rowBytes * NUM_IMAGE_ROWS)
								{
									for (DWORD row = 0; row < NUM_IMAGE_ROWS; row++)
									{
										CopyMemory(scanline + static_cast<ptrdiff_t>(row) * pitch,
											static_cast<BYTE*>(dibPixels) + row * rowBytes, rowBytes);
									}
								}
							}
						}

						if (hOld)
							SelectObject(hdcMem, hOld);
						if (hBitmap)
							DeleteObject(hBitmap);
						if (hdcMem)
							DeleteDC(hdcMem);
						if (hdcWindow)
							ReleaseDC(obsWindow, hdcWindow);
					}
				}

				buffer2D->Unlock2D();
			}
		}
	}
	// The source DLL is hosted by Camera Frame Server and cannot see windows on
	// the interactive desktop. VCamSample.exe captures OBS there and publishes a
	// complete frame through this file-backed shared mapping.
	DWORD sharedMappingError = ERROR_SUCCESS;
	DWORD sharedViewError = ERROR_SUCCESS;
	static volatile LONG sharedStatusLogged = 0;
	HANDLE sharedLog = INVALID_HANDLE_VALUE;
	if (InterlockedCompareExchange(&sharedStatusLogged, 1, 0) == 0)
	{
		sharedLog = CreateFileW(L"C:\\Users\\Public\\VCamSampleReader.log", FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	}
	auto logStage = [sharedLog](char stage)
		{
			if (sharedLog != INVALID_HANDLE_VALUE)
			{
				DWORD written = 0;
				WriteFile(sharedLog, &stage, 1, &written, nullptr);
				FlushFileBuffers(sharedLog);
			}
		};
	logStage('A');
	if (!_sharedFrame)
	{
		_sharedFrameMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, VCAM_SHARED_FRAME_NAME);
		if (!_sharedFrameMapping)
			sharedMappingError = GetLastError();
		if (_sharedFrameMapping)
		{
			_sharedFrame = static_cast<SharedFrame*>(MapViewOfFile(_sharedFrameMapping,
				FILE_MAP_READ, 0, 0, sizeof(SharedFrame)));
			if (!_sharedFrame)
				sharedViewError = GetLastError();
		}
	}
	logStage('B');

	const bool sharedHeaderValid = _sharedFrame &&
		_sharedFrame->magic == VCAM_SHARED_FRAME_MAGIC &&
		_sharedFrame->version == VCAM_SHARED_FRAME_VERSION &&
		_sharedFrame->width == NUM_IMAGE_COLS &&
		_sharedFrame->height == NUM_IMAGE_ROWS &&
		_sharedFrame->stride == NUM_IMAGE_COLS * 4 &&
		_sharedFrame->capturedAtTick != 0 &&
		GetTickCount64() - _sharedFrame->capturedAtTick < 2000;
	logStage('C');
	HRESULT sharedGetBufferResult = E_PENDING;
	HRESULT sharedQueryResult = E_PENDING;
	HRESULT sharedLockResult = E_PENDING;
	bool sharedCopyCompleted = false;
	if (sharedHeaderValid)
	{
		wil::com_ptr_nothrow<IMFMediaBuffer> sharedMediaBuffer;
		wil::com_ptr_nothrow<IMF2DBuffer2> sharedBuffer2D;
		sharedGetBufferResult = outSample->GetBufferByIndex(0, &sharedMediaBuffer);
		logStage('D');
		if (SUCCEEDED(sharedGetBufferResult))
			sharedQueryResult = sharedMediaBuffer->QueryInterface(IID_PPV_ARGS(&sharedBuffer2D));
		logStage('E');
		if (SUCCEEDED(sharedGetBufferResult) && SUCCEEDED(sharedQueryResult))
		{
			logStage('F');
			logStage('G');
			for (int attempt = 0; attempt < 3; attempt++)
			{
				LONG sequenceBefore = InterlockedCompareExchange(&_sharedFrame->sequence, 0, 0);
				if (sequenceBefore & 1)
				{
					SwitchToThread();
					continue;
				}

				MemoryBarrier();
				sharedLockResult = sharedBuffer2D->ContiguousCopyFrom(
					_sharedFrame->pixels, sizeof(_sharedFrame->pixels));
				MemoryBarrier();

				LONG sequenceAfter = InterlockedCompareExchange(&_sharedFrame->sequence, 0, 0);
				if (SUCCEEDED(sharedLockResult) && sequenceBefore == sequenceAfter && !(sequenceAfter & 1))
				{
					sharedCopyCompleted = true;
					break;
				}
			}
			logStage('H');
		}
	}
	if (sharedLog != INVALID_HANDLE_VALUE)
		CloseHandle(sharedLog);
	// --- END OF HOOK ---

	if (pToken)
	{
		RETURN_IF_FAILED(outSample->SetUnknown(MFSampleExtension_Token, pToken));
	}
	RETURN_IF_FAILED(_queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, outSample.get()));
	return S_OK;
}


// IMFMediaStream2
STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE value)
{
	WINTRACE(L"MediaStream::SetStreamState current:%u value:%u", _state, value);
	if (_state = value)
		return S_OK;
	switch (value)
	{
	case MF_STREAM_STATE_PAUSED:
		if (_state != MF_STREAM_STATE_RUNNING)
			RETURN_HR(MF_E_INVALID_STATE_TRANSITION);

		_state = value;
		break;

	case MF_STREAM_STATE_RUNNING:
		RETURN_IF_FAILED(Start(nullptr));
		break;

	case MF_STREAM_STATE_STOPPED:
		RETURN_IF_FAILED(Stop());
		break;

	default:
		RETURN_HR(MF_E_INVALID_STATE_TRANSITION);
		break;
	}
	return S_OK;
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* value)
{
	WINTRACE(L"MediaStream::GetStreamState state:%u", _state);
	RETURN_HR_IF_NULL(E_POINTER, value);
	*value = _state;
	return S_OK;
}

// IKsControl
STDMETHODIMP_(NTSTATUS) MediaStream::KsProperty(PKSPROPERTY property, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	WINTRACE(L"MediaStream::KsProperty len:%u data:%p dataLength:%u", length, data, dataLength);
	RETURN_HR_IF_NULL(E_POINTER, property);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	WINTRACE(L"MediaStream::KsProperty prop:%s", PKSIDENTIFIER_ToString(property, length).c_str());

	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaStream::KsMethod(PKSMETHOD method, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	WINTRACE(L"MediaStream::KsMethod len:%u data:%p dataLength:%u", length, data, dataLength);
	RETURN_HR_IF_NULL(E_POINTER, method);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	WINTRACE(L"MediaStream::KsMethod method:%s", PKSIDENTIFIER_ToString(method, length).c_str());

	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaStream::KsEvent(PKSEVENT evt, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	WINTRACE(L"MediaStream::KsEvent evt:%p len:%u data:%p dataLength:%u", evt, length, data, dataLength);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	WINTRACE(L"MediaStream::KsEvent event:%s", PKSIDENTIFIER_ToString(evt, length).c_str());
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
