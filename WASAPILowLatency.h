#pragma once

#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cmath>
#include <atomic>
#include <thread>

enum class LatencyMode {
	Safe,
	Aggressive
};

class WASAPIContext {
public:
	// For absolute minimal latency, we do not use std::function. Might be overthinking, but...
	typedef void (*RenderCallback)(float *dest, int framesToWrite, int channels, int sampleRate, void *userdata);

	WASAPIContext(RenderCallback callback, void *userdata);
	~WASAPIContext();

	void EnumerateOutputDevices();
	bool InitializeDevice(LatencyMode latencyMode);
	int PeriodFrames() const { return actualPeriodFrames_; }
	float FramesToMs(int frames) const {
		return 1000.0f * frames / format_->nSamplesPerSec;
	}

	// Implements device change notifications
	class DeviceNotificationClient : public IMMNotificationClient {
	public:
		DeviceNotificationClient(WASAPIContext *engine) : engine_(engine) {}
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
		WASAPIContext *engine_;
	};

private:
	void Start();
	void Stop();

	void AudioLoop();

	// Only one of these can be non-null at a time. Check audioClient3 to determine if it's being used.
	IAudioClient3 *audioClient3_ = nullptr;
	IAudioClient *audioClient_ = nullptr;

	IAudioRenderClient* renderClient_ = nullptr;
	WAVEFORMATEX* format_ = nullptr;
	HANDLE audioEvent_ = nullptr;
	std::thread audioThread_;
	UINT32 defaultPeriodFrames = 0, fundamentalPeriodFrames = 0, minPeriodFrames = 0, maxPeriodFrames = 0;
	std::atomic<bool> running_ = true;
	UINT32 actualPeriodFrames_ = 0;  // may not be the requested.
	IMMDeviceEnumerator *enumerator_ = nullptr;
	DeviceNotificationClient notificationClient_;
	RenderCallback callback_{};
	void *userdata_ = nullptr;
	LatencyMode latencyMode_ = LatencyMode::Aggressive;
};