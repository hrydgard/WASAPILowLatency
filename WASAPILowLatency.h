#pragma once

#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cmath>
#include <atomic>
#include <thread>

constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNELS = 2;
constexpr double FREQUENCY = 440.0;

extern std::atomic<bool> playTone;

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

	void InitializeDevice(LatencyMode latencyMode);

	void Start();
	void Stop();

	void AudioLoop();

	int PeriodFrames() const { return actualPeriodFrames_; }

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