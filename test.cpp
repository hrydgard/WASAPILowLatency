#include "WASAPILowLatency.h"
#include <cmath>

std::atomic<bool> playTone = false;

LRESULT CALLBACK WndProc(HWND hwnd_, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_KEYDOWN:
		if (wParam == VK_SPACE)
			playTone = true;
		break;
	case WM_KEYUP:
		if (wParam == VK_SPACE)
			playTone = false;
		break;
	case WM_LBUTTONDOWN:
		playTone = true;
		break;
	case WM_LBUTTONUP:
		playTone = false;
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	}
	return DefWindowProc(hwnd_, msg, wParam, lParam);
}

constexpr double FREQUENCY = 440.0;
constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNELS = 2;

int main() {
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	const HINSTANCE hInstance = GetModuleHandle(NULL);
	WNDCLASS wc{};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = L"ToneWindowClass";
	RegisterClass(&wc);

	const HWND hwnd = CreateWindowEx(
		0, L"ToneWindowClass", L"Click, or press space",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
		500, 500, nullptr, nullptr, hInstance, nullptr
	);

	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);

	WASAPIContext *engine = new WASAPIContext();
	engine->SetRenderCallback([](float *dest, int framesToWrite, int channels, int sampleRateHz, void *userdata) {
		static double phase = 0.0;

		float* fBuffer = dest;
		double phaseStep = 2.0 * 3.14159265358979323846 * FREQUENCY / SAMPLE_RATE;

		for (int i = 0; i < framesToWrite; ++i) {
			float sample = playTone ? static_cast<float>(sin(phase)) : 0.0f;
			phase += phaseStep;
			if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;

			for (int c = 0; c < channels; ++c) {
				*fBuffer++ = sample;
			}
		}

		static int count = 0;
		count++;
		if (count < 10) {
			printf("%d framesToWrite\n", framesToWrite);
		}
	}, nullptr);

	std::vector<AudioDeviceDesc> devices;
	engine->EnumerateOutputDevices(&devices);
	for (auto &device : devices) {
		printf("%s: %s\n", device.name.c_str(), device.uniqueId.c_str());
	}

	if (!engine->InitializeDevice("", LatencyMode::Aggressive)) {
		printf("Failed to initialize audio\n");
		return 1;
	}
	printf("Initialized audio, got %0.2f (period: %d buffer: %d)\n", engine->FramesToMs(engine->PeriodFrames()), engine->PeriodFrames(), engine->BufferSize());

	MSG msg = {};
	while (GetMessage(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	delete engine;

	CoUninitialize();
	return 0;
}