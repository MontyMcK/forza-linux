// Audio output: one stream of interleaved PCM frames to the default playback device.
#pragma once

#include <cstddef>

namespace fl::os {

enum class SampleFormat { F32, S16, S32 };

struct AudioOut;

// Opens the default playback device for `channels` x `rate` Hz frames of
// `format`. nullptr when there's no audio (the reason is logged).
// *device_frames: frames the device pulls at a time (0 if unknown).
AudioOut* audio_open(SampleFormat format, int channels, int rate, int* device_frames);
void audio_close(AudioOut* out);

size_t audio_queued_bytes(AudioOut* out);  // written but not played yet
void audio_write(AudioOut* out, const void* data, size_t bytes);
void audio_pause(AudioOut* out);
void audio_resume(AudioOut* out);
void audio_clear(AudioOut* out);  // drops what's queued

}  // namespace fl::os
