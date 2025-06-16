#include "WASAPILowLatency.h"

LRESULT CALLBACK WndProc(HWND hwnd_, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_KEYDOWN:
		if (wParam == VK_SPACE) playTone = true;
		break;
	case WM_KEYUP:
		if (wParam == VK_SPACE) playTone = false;
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

int main() {
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	HINSTANCE hInstance = GetModuleHandle(NULL);
	WNDCLASS wc = { };
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = L"ToneWindowClass";
	RegisterClass(&wc);

	HWND hwnd = CreateWindowEx(
		0, L"ToneWindowClass", L"Click, or press space",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
		500, 500, nullptr, nullptr, hInstance, nullptr
	);

	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);

	WASAPIContext *engine = new WASAPIContext([](float *dest, int frames, int channels, int sampleRateHz, void *userdata) {

	}, nullptr);

	engine->EnumerateOutputDevices();
	engine->InitializeDevice(LatencyMode::Aggressive);

	MSG msg = {};
	while (GetMessage(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	delete engine;

	CoUninitialize();
	return 0;
}