#pragma once

#include <vector>
#include <string>
#include <string_view>

// For absolute minimal latency, we do not use std::function. Might be overthinking, but...
typedef void (*RenderCallback)(float *dest, int framesToWrite, int channels, int sampleRate, void *userdata);

enum class LatencyMode {
	Safe,
	Aggressive
};

struct AudioDeviceDesc {
	std::string name;      // User-friendly name
	std::string uniqueId;  // store-able ID for settings.
};

inline float FramesToMs(int frames, int sampleRate) {
	return 1000.0f * (float)frames / (float)sampleRate;
}

class AudioBackend {
public:
	virtual ~AudioBackend() {}
	virtual void EnumerateDevices(std::vector<AudioDeviceDesc> *outputDevices, bool captureDevices = false) = 0;
	virtual void SetRenderCallback(RenderCallback callback, void *userdata) = 0;
	virtual bool InitOutputDevice(std::string_view uniqueId, LatencyMode latencyMode) = 0;
	virtual int SampleRate() const = 0;
	virtual int BufferSize() const = 0;
	virtual int PeriodFrames() const = 0;
	virtual void FrameUpdate(bool allowAutoChange) {}
};

AudioBackend *CreateWASAPI();