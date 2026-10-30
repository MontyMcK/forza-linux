// PS5 audio output: SceAudioOut, not written yet. Until then the game runs
// silent (WASAPI treats a null output as the null backend).
#include "platform/os/audio.h"

#include "common/log.h"

namespace fl::os {

AudioOut* audio_open(SampleFormat, int, int, int* device_frames) {
    *device_frames = 0;
    FL_ERROR("UNIMPLEMENTED os::audio_open on PS5 (SceAudioOut, PS5_PORT.md M6): no sound");
    return nullptr;
}
void audio_close(AudioOut*) {}
size_t audio_queued_bytes(AudioOut*) { return 0; }
void audio_write(AudioOut*, const void*, size_t) {}
void audio_pause(AudioOut*) {}
void audio_resume(AudioOut*) {}
void audio_clear(AudioOut*) {}

}  // namespace fl::os
