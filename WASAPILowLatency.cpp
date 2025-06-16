#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <avrt.h>
#include <comdef.h>
#include <atomic>
#include <thread>
#include <iostream>
#include <vector>

#include "WASAPILowLatency.h"

WASAPIContext::WASAPIContext(WASAPIContext::RenderCallback callback, void *userdata) : notificationClient_(this), callback_(callback), userdata_(userdata) {
	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator_);
	if (FAILED(hr)) {
		// Bad!
		enumerator_ = nullptr;
		return;
	}
	enumerator_->RegisterEndpointNotificationCallback(&notificationClient_);
}

WASAPIContext::~WASAPIContext() {
	if (!enumerator_) {
		// Nothing can have been happening.
		return;
	}
	Stop();
	enumerator_->UnregisterEndpointNotificationCallback(&notificationClient_);
	enumerator_->Release();
}

void WASAPIContext::EnumerateOutputDevices() {
	IMMDeviceCollection *collection = nullptr;
	enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);

	UINT count = 0;
	collection->GetCount(&count);

	for (UINT i = 0; i < count; ++i) {
		IMMDevice *device = nullptr;
		collection->Item(i, &device);

		IPropertyStore *props = nullptr;
		device->OpenPropertyStore(STGM_READ, &props);

		PROPVARIANT nameProp;
		PropVariantInit(&nameProp);
		props->GetValue(PKEY_Device_FriendlyName, &nameProp);

		LPWSTR id_str = 0;
		if (SUCCEEDED(device->GetId(&id_str))) {
			wprintf(L"Device %u: %s (id: %s)\n", i, nameProp.pwszVal, id_str);
			CoTaskMemFree(id_str);
		}

		PropVariantClear(&nameProp);
		props->Release();
		device->Release();
	}

	collection->Release();
}

bool WASAPIContext::InitializeDevice(LatencyMode latencyMode) {
	Stop();

	IMMDevice* device = nullptr;

	// enumerator->GetDevice
	enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device);

	// Try IAudioClient3 first
	HRESULT hr = E_FAIL;
	// It's probably safe anyway, but still, let's use the legacy client as a safe fallback option.
	if (latencyMode != LatencyMode::Safe) {
		hr = device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void**)&audioClient3_);
	}
	if (SUCCEEDED(hr)) {
		audioClient3_->GetMixFormat(&format_);
		audioClient3_->GetSharedModeEnginePeriod(format_, &defaultPeriodFrames, &fundamentalPeriodFrames, &minPeriodFrames, &maxPeriodFrames);

		printf("default: %d fundamental: %d min: %d max: %d\n", defaultPeriodFrames, fundamentalPeriodFrames, minPeriodFrames, maxPeriodFrames);
		printf("initializing with %d frame period at %d Hz, meaning %0.1fms\n", (int)minPeriodFrames, (int)format_->nSamplesPerSec, FramesToMs(minPeriodFrames));

		audioEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		HRESULT result = audioClient3_->InitializeSharedAudioStream(
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
			minPeriodFrames,
			format_,
			nullptr
		);
		if (FAILED(result)) {
			printf("Error initializing shared audio stream: %08x", result);
			audioClient3_->Release();
			device->Release();
			audioClient3_ = nullptr;
			device = nullptr;
			return false;
		}

		audioClient3_->SetEventHandle(audioEvent_);
		audioClient3_->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient_);
	} else {
		// Fallback to IAudioClient (older OS)
		device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient_);

		audioClient_->GetMixFormat(&format_);

		// Get engine period info
		REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
		audioClient_->GetDevicePeriod(&defaultPeriod, &minPeriod);

		audioEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);

		const REFERENCE_TIME duration = minPeriod;
		HRESULT hr = audioClient_->Initialize(
			AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
			duration,  // This is a minimum, the result might be larger. We use GetBufferSize to check.
			0,  // ref duration, always 0 in shared mode.
			format_,
			nullptr
		);

		if (FAILED(hr)) {
			printf("ERROR: Failed to initialize audio with all attempted buffer sizes\n");
			audioClient_->Release();
			device->Release();
			audioClient_ = nullptr;
			device = nullptr;
			return false;
		}
		audioClient_->GetBufferSize(&actualPeriodFrames_);
		audioClient_->SetEventHandle(audioEvent_);
		audioClient_->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient_);
	}

	latencyMode_ = latencyMode;

	Start();

	device->Release();
	return true;
}

void WASAPIContext::Start() {
	running_ = true;
	audioThread_ = std::thread([this]() { AudioLoop(); });
}

void WASAPIContext::Stop() {
	running_ = false;
	if (audioClient_) audioClient_->Stop();
	if (audioEvent_) SetEvent(audioEvent_);
	if (audioThread_.joinable()) audioThread_.join();

	if (renderClient_) { renderClient_->Release(); renderClient_ = nullptr; }
	if (audioClient_) { audioClient_->Release(); audioClient_ = nullptr; }
	if (audioEvent_) { CloseHandle(audioEvent_); audioEvent_ = nullptr; }
	if (format_) { CoTaskMemFree(format_); format_ = nullptr; }
}

void WASAPIContext::AudioLoop() {
	DWORD taskID = 0;
	HANDLE mmcssHandle = AvSetMmThreadCharacteristics(L"Pro Audio", &taskID);

	if (audioClient3_)
		audioClient3_->Start();
	else
		audioClient_->Start();

	double phase = 0.0;

	while (running_) {
		DWORD result = WaitForSingleObject(audioEvent_, INFINITE);
		if (result != WAIT_OBJECT_0) break;

		UINT32 padding = 0, available = 0;
		if (audioClient3_)
			audioClient3_->GetCurrentPadding(&padding), audioClient3_->GetBufferSize(&available);
		else
			audioClient_->GetCurrentPadding(&padding), audioClient_->GetBufferSize(&available);

		UINT32 framesToWrite = available - padding;
		BYTE* buffer = nullptr;
		if (framesToWrite > 0 && SUCCEEDED(renderClient_->GetBuffer(framesToWrite, &buffer))) {
			callback_((float *)buffer, framesToWrite, 2, format_->nSamplesPerSec, userdata_);
			renderClient_->ReleaseBuffer(framesToWrite, 0);
		}
	}

	if (audioClient3_) {
		audioClient3_->Stop();
	} else {
		audioClient_->Stop();
	}

	if (mmcssHandle) {
		AvRevertMmThreadCharacteristics(mmcssHandle);
	}
}