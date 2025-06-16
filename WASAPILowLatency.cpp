#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <avrt.h>
#include <comdef.h>
#include <cmath>
#include <atomic>
#include <thread>
#include <iostream>
#include <vector>

#include "WASAPILowLatency.h"

std::atomic<bool> playTone = false;

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

void WASAPIContext::InitializeDevice(LatencyMode latencyMode) {
	IMMDevice* device = nullptr;

	// enumerator->GetDevice
	enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device);

	// Try IAudioClient3 first
	HRESULT hr = E_FAIL;
	if (latencyMode != LatencyMode::Safe) {
		// It's probably safe anyway, but still, let's use the legacy client as a fallback option.
		hr = device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void**)&audioClient3);
	}
	if (SUCCEEDED(hr)) {
		audioClient3->GetMixFormat(&format_);
		audioClient3->GetSharedModeEnginePeriod(format_, &defaultPeriodFrames, &fundamentalPeriodFrames, &minPeriodFrames, &maxPeriodFrames);

		printf("default: %d fundamental: %d min: %d max: %d\n", defaultPeriodFrames, fundamentalPeriodFrames, minPeriodFrames, maxPeriodFrames);
		printf("initializing with %d frame period at %d Hz, meaning %0.1fms\n", (int)minPeriodFrames, (int)format_->nSamplesPerSec, FramesToMs(minPeriodFrames));

		audioEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		HRESULT result = audioClient3->InitializeSharedAudioStream(
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
			minPeriodFrames,
			format_,
			nullptr
		);
		if (FAILED(result)) {
			printf("Error initializing shared audio stream: %08x", result);
			device->Release();
			return;
		}

		audioClient3->SetEventHandle(audioEvent_);
		audioClient3->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
	} else {
		// Fallback to IAudioClient (older OS)
		device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);

		audioClient->GetMixFormat(&format_);

		// Get engine period info
		REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
		audioClient->GetDevicePeriod(&defaultPeriod, &minPeriod);

		audioEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);

		const REFERENCE_TIME duration = minPeriod;
		// Try each duration until one succeeds
		bool initialized = false;
		HRESULT hr = audioClient->Initialize(
			AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
			duration,
			0,  // ref duration, always 0 in shared mode.
			format_,
			nullptr
		);

		if (FAILED(hr)) {
			printf("ERROR: Failed to initialize audio with all attempted buffer sizes\n");
			return;
		}
		audioClient->GetBufferSize(&actualPeriodFrames_);
		printf("Initialized audio, requested %.2f ms buffer but got %.2f\n", duration / 10000.0, FramesToMs(actualPeriodFrames_));
		initialized = true;
		audioClient->SetEventHandle(audioEvent_);
		audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
	}

	Start();

	device->Release();
}

void WASAPIContext::Start() {
	audioThread = std::thread([this]() { AudioLoop(); });
}

void WASAPIContext::Stop() {
	running = false;
	if (audioClient) audioClient->Stop();
	if (audioEvent_) SetEvent(audioEvent_);
	if (audioThread.joinable()) audioThread.join();

	if (renderClient) { renderClient->Release(); renderClient = nullptr; }
	if (audioClient) { audioClient->Release(); audioClient = nullptr; }
	if (audioEvent_) { CloseHandle(audioEvent_); audioEvent_ = nullptr; }
	if (format_) { CoTaskMemFree(format_); format_ = nullptr; }
}

void WASAPIContext::AudioLoop() {
	DWORD taskID = 0;
	HANDLE mmcssHandle = AvSetMmThreadCharacteristics(L"Pro Audio", &taskID);

	if (audioClient3)
		audioClient3->Start();
	else
		audioClient->Start();

	double phase = 0.0;

	while (running) {
		DWORD result = WaitForSingleObject(audioEvent_, INFINITE);
		if (result != WAIT_OBJECT_0) break;

		UINT32 padding = 0, available = 0;
		if (audioClient3)
			audioClient3->GetCurrentPadding(&padding), audioClient3->GetBufferSize(&available);
		else
			audioClient->GetCurrentPadding(&padding), audioClient->GetBufferSize(&available);

		UINT32 framesToWrite = available - padding;
		BYTE* buffer = nullptr;
		if (framesToWrite > 0 &&
			SUCCEEDED(renderClient->GetBuffer(framesToWrite, &buffer))) {
			FillSineWave(buffer, framesToWrite, phase);
			renderClient->ReleaseBuffer(framesToWrite, 0);
		}
	}

	if (audioClient3) {
		audioClient3->Stop();
	} else {
		audioClient->Stop();
	}

	if (mmcssHandle) {
		AvRevertMmThreadCharacteristics(mmcssHandle);
	}
}