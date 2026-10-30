// Audio output through an SDL3 audio stream (PipeWire on the user's machine).
#include "platform/os/audio.h"

#include <algorithm>
#include <cstdlib>

#include <SDL3/SDL.h>

#include "common/log.h"

namespace fl::os {

struct AudioOut {
    SDL_AudioStream* stream;
};

AudioOut* audio_open(SampleFormat format, int channels, int rate, int* device_frames) {
    *device_frames = 0;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        FL_ERROR("audio: SDL_InitSubSystem(AUDIO) failed: %s (sound disabled)", SDL_GetError());
        return nullptr;
    }
    SDL_AudioFormat sf = format == SampleFormat::F32 ? SDL_AUDIO_F32 : format == SampleFormat::S16 ? SDL_AUDIO_S16 : SDL_AUDIO_S32;
    SDL_AudioSpec spec{sf, channels, rate};
    // Ask for ~10 ms device periods (PipeWire's default quantum here is
    // 1024 frames, 21 ms); FL_AUDIO_FRAMES overrides.
    const char* frames_env = getenv("FL_AUDIO_FRAMES");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames_env ? frames_env : "480");
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
        FL_ERROR("audio: SDL_OpenAudioDeviceStream failed: %s (sound disabled)", SDL_GetError());
        return nullptr;
    }
    SDL_AudioSpec dev{};
    if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &dev, device_frames)) *device_frames = 0;
    fl::log_write("audio", "SDL audio stream open (%s), device period %d frames", SDL_GetCurrentAudioDriver(),
                  *device_frames);
    return new AudioOut{stream};
}

void audio_close(AudioOut* out) {
    if (!out) return;
    SDL_DestroyAudioStream(out->stream);
    delete out;
}

size_t audio_queued_bytes(AudioOut* out) { return static_cast<size_t>(std::max(0, SDL_GetAudioStreamQueued(out->stream))); }

void audio_write(AudioOut* out, const void* data, size_t bytes) {
    SDL_PutAudioStreamData(out->stream, data, static_cast<int>(bytes));
}

void audio_pause(AudioOut* out) { SDL_PauseAudioStreamDevice(out->stream); }
void audio_resume(AudioOut* out) { SDL_ResumeAudioStreamDevice(out->stream); }
void audio_clear(AudioOut* out) { SDL_ClearAudioStream(out->stream); }

}  // namespace fl::os
