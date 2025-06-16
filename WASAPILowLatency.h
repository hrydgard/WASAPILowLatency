#pragma once

#include "AudioBackend.h"

#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cmath>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <string_view>

class WASAPIContext : public AudioBackend {
public:
	WASAPIContext();
	~WASAPIContext();

	void SetRenderCallback(RenderCallback callback, void *userdata) override {
		callback_ = callback;
		userdata_ = userdata;
	}

	void EnumerateDevices(std::vector<AudioDeviceDesc> *outputDevices, bool captureDevices = false);

	bool InitOutputDevice(std::string_view uniqueId, LatencyMode latencyMode);

	void FrameUpdate(bool allowAutoChange) override;

	int PeriodFrames() const override { return actualPeriodFrames_; }  // NOTE: This may have the wrong value (too large) until audio has started playing.
	int BufferSize() const override { return reportedBufferSize_; }
	int SampleRate() const override { return format_->nSamplesPerSec; }

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
				engine_->defaultDeviceChanged_ = true;
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
	WAVEFORMATEX *format_ = nullptr;
	HANDLE audioEvent_ = nullptr;
	std::thread audioThread_;
	UINT32 defaultPeriodFrames = 0, fundamentalPeriodFrames = 0, minPeriodFrames = 0, maxPeriodFrames = 0;
	std::atomic<bool> running_ = true;
	UINT32 actualPeriodFrames_ = 0;  // may not be the requested.
	UINT32 reportedBufferSize_ = 0;
	IMMDeviceEnumerator *enumerator_ = nullptr;
	DeviceNotificationClient notificationClient_;
	RenderCallback callback_{};
	void *userdata_ = nullptr;
	LatencyMode latencyMode_ = LatencyMode::Aggressive;
	std::string deviceId_;
	std::atomic<bool> defaultDeviceChanged_{};
};