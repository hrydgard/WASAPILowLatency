#pragma once

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

constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNELS = 2;
constexpr double FREQUENCY = 440.0;

extern std::atomic<bool> playTone;

enum class LatencyMode {
	Safe,
	Aggressive
};

typedef void (*RenderCallback)(float *dest, int framesToWrite, int channels, int sampleRate, void *userdata);

class AudioEngine {
public:
	AudioEngine(RenderCallback callback, void *userdata) : notificationClient_(this), callback_(callback), userdata_(userdata) {
		HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator_);
		if (FAILED(hr)) {
			// Bad!
			enumerator_ = nullptr;
			return;
		}
		enumerator_->RegisterEndpointNotificationCallback(&notificationClient_);
	}
	~AudioEngine() {
		if (!enumerator_) {
			// Nothing can have been happening.
			return;
		}
		Stop();
		enumerator_->UnregisterEndpointNotificationCallback(&notificationClient_);
		enumerator_->Release();
	}
	void EnumerateOutputDevices() {
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

	void InitializeDevice(LatencyMode latencyMode) {
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

	void Start() {
		audioThread = std::thread([this]() { AudioLoop(); });
	}

	void Stop() {
		running = false;
		if (audioClient) audioClient->Stop();
		if (audioEvent_) SetEvent(audioEvent_);
		if (audioThread.joinable()) audioThread.join();

		if (renderClient) { renderClient->Release(); renderClient = nullptr; }
		if (audioClient) { audioClient->Release(); audioClient = nullptr; }
		if (audioEvent_) { CloseHandle(audioEvent_); audioEvent_ = nullptr; }
		if (format_) { CoTaskMemFree(format_); format_ = nullptr; }
	}

	void AudioLoop() {
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

	int PeriodFrames() const {
		return actualPeriodFrames_;
	}

	void FillSineWave(BYTE* buffer, UINT32 frames, double& phase) {
		float* fBuffer = reinterpret_cast<float*>(buffer);
		double phaseStep = 2.0 * 3.14159265358979323846 * FREQUENCY / SAMPLE_RATE;

		for (UINT32 i = 0; i < frames; ++i) {
			float sample = playTone ? static_cast<float>(sin(phase)) : 0.0f;
			phase += phaseStep;
			if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;

			for (int c = 0; c < CHANNELS; ++c) {
				*fBuffer++ = sample;
			}
		}
	}

	// Implements device change notifications
	class DeviceNotificationClient : public IMMNotificationClient {
	public:
		DeviceNotificationClient(AudioEngine *engine) : engine_(engine) {}
		ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
		ULONG STDMETHODCALLTYPE Release() override { return 1; }
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
			if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
				*ppv = static_cast<IMMNotificationClient*>(this);
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}

		HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
			if (flow == eRender && role == eConsole) {
				// PostMessage(hwnd, WM_APP + 1, 0, 0);
			}
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
	private:
		AudioEngine *engine_;
	};

private:
	void RegisterDeviceNotifications();
	void UnregisterDeviceNotifications();

	float FramesToMs(int frames) const {
		return 1000.0f * frames / format_->nSamplesPerSec;
	}

	// Only one of these can be non-null at a time. Check audioClient3 to determine if it's being used.
	IAudioClient3 *audioClient3 = nullptr;
	IAudioClient *audioClient = nullptr;

	IAudioRenderClient* renderClient = nullptr;
	WAVEFORMATEX* format_ = nullptr;
	HANDLE audioEvent_ = nullptr;
	std::thread audioThread;
	UINT32 defaultPeriodFrames = 0, fundamentalPeriodFrames = 0, minPeriodFrames = 0, maxPeriodFrames = 0;
	std::atomic<bool> running = true;
	UINT32 actualPeriodFrames_ = 0;  // may not be the requested.
	IMMDeviceEnumerator *enumerator_ = nullptr;
	DeviceNotificationClient notificationClient_;
	RenderCallback callback_{};
	void *userdata_ = nullptr;
};