#include "WASAPILowLatency.h"
#include "AudioBackend.h"

AudioBackend *CreateWASAPI() {
	return new WASAPIContext();
}