// The DirectSound mixer's output (sdk/runtime/xdk/dsound.cpp Render: 48 kHz interleaved stereo float) played through
// SDL3, in place of the headless harness's null output (sdk/harness/null_audio.cpp, kept for headless builds).
//
// SDL's audio thread asks for data as the device drains it; the mixer renders exactly that much, in pieces of at most
// 10 ms (the granularity the null output and the XAudio2 output use), so its state advances with the device clock.
//   CSX_AUDIO=null      no device: the mixer is pulled every 10 ms and its output dropped, as headless
//   CSX_AUDIO_WAV=PATH  everything sent to the device also written to PATH (48 kHz stereo float WAV), for checking
//                       levels, clipping and stereo placement without listening; continued across a relaunch (delete
//                       the file before a new capture)
//   CSX_AUDIO_MS=N      device buffer request in milliseconds (default 20)
// Without a device (no audio server) the null pump takes over, so the mixer and its callbacks keep running.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio.h"
#include "common/pc_settings.h"
#include "core/log.h"

namespace audio {

namespace {

std::function<void(float*, uint32_t)> g_render;
FILE* g_wav = nullptr;
uint64_t g_wav_frames = 0;
std::mutex g_wav_lock;
// late device callbacks: a gap between two longer than twice the device period, when the device may have run dry
std::atomic<uint64_t> g_late{0}, g_callbacks{0};
double g_period_ms = 20;

void WavHeader(FILE* f, uint64_t frames) {
    uint32_t data = uint32_t(std::min<uint64_t>(frames * 8, 0xFFFFFFF0u));
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fseek(f, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + data);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(3);  // IEEE float
    u16(2);
    u32(kRate);
    u32(kRate * 8);
    u16(8);
    u16(32);
    std::fwrite("data", 1, 4, f);
    u32(data);
    std::fseek(f, 0, SEEK_END);
}

void Capture(const float* samples, uint32_t frames) {
    if (!g_wav) return;
    std::lock_guard<std::mutex> lock(g_wav_lock);
    std::fwrite(samples, 8, frames, g_wav);
    g_wav_frames += frames;
    if (g_wav_frames % (kRate * 2) < frames) {  // the header kept valid every couple of seconds
        WavHeader(g_wav, g_wav_frames);
        std::fflush(g_wav);
    }
}

// Options > PC Options: Master Volume (0-100, default 100) on what goes to the device, after the game's own mix; a
// split-screen player's stream also at its share (CSX_AUDIO_GAIN, the compositor's 1/sqrt(players): the system mixes
// every player's stream, each heard from that player's own position)
void Volume(float* samples, uint32_t frames) {
    static unsigned seen = ~0u;
    static float gain = 1.0f;
    static const float share = [] {
        const char* g = std::getenv("CSX_AUDIO_GAIN");
        float v = g ? float(std::atof(g)) : 1.0f;
        return v > 0.0f && v <= 1.0f ? v : 1.0f;
    }();
    if (pc::Version() != seen) {
        seen = pc::Version();
        gain = std::clamp(pc::GetFloat("master_volume", 100.0f), 0.0f, 100.0f) / 100.0f * share;
    }
    if (gain != 1.0f)
        for (uint32_t i = 0; i < frames * 2; i++) samples[i] *= gain;
}

// SDL's audio thread: render what the device asks for
void SDLCALL Feed(void*, SDL_AudioStream* stream, int additional, int) {
    static std::vector<float> buf;
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (g_callbacks++ && std::chrono::duration<double, std::milli>(now - last).count() > 2 * g_period_ms) g_late++;
    last = now;
    uint32_t frames = uint32_t(additional) / 8;
    while (frames) {
        uint32_t n = std::min<uint32_t>(frames, kRate / 100);
        buf.assign(size_t(n) * 2, 0.0f);
        g_render(buf.data(), n);
        Volume(buf.data(), n);
        Capture(buf.data(), n);
        SDL_PutAudioStreamData(stream, buf.data(), int(n * 8));
        frames -= n;
    }
}

void NullPump() {
    std::thread([] {
        std::vector<float> b(kRate / 100 * 2);
        auto next = std::chrono::steady_clock::now();
        for (;;) {
            std::fill(b.begin(), b.end(), 0.0f);
            g_render(b.data(), kRate / 100);
            Volume(b.data(), kRate / 100);
            Capture(b.data(), kRate / 100);
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
        }
    }).detach();
}

}  // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    g_render = std::move(render);
    if (const char* wav = std::getenv("CSX_AUDIO_WAV"); wav && *wav) {
        // a title relaunch (front end to match) runs a new process: an existing capture of this format is continued
        g_wav = std::fopen(wav, "r+b");
        if (g_wav) {
            std::fseek(g_wav, 0, SEEK_END);
            long size = std::ftell(g_wav);
            g_wav_frames = size > 44 ? uint64_t(size - 44) / 8 : 0;
        } else if ((g_wav = std::fopen(wav, "wb"))) {
            WavHeader(g_wav, 0);
        }
        LOGI("audio: capturing the output to %s", wav);
    }
    const char* mode = std::getenv("CSX_AUDIO");
    if (mode && std::strcmp(mode, "null") == 0) {
        LOGI("audio: null output (CSX_AUDIO=null)");
        NullPump();
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOGW("audio: SDL audio unavailable (%s), null output", SDL_GetError());
        NullPump();
        return;
    }
    int ms = std::clamp(std::getenv("CSX_AUDIO_MS") ? std::atoi(std::getenv("CSX_AUDIO_MS")) : 20, 5, 200);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(kRate * ms / 1000).c_str());
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, int(kRate)};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Feed, nullptr);
    if (!stream) {
        LOGW("audio: no playback device (%s), null output", SDL_GetError());
        NullPump();
        return;
    }
    SDL_AudioSpec dev{};
    int dev_frames = 0;
    SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &dev, &dev_frames);
    LOGI("audio: SDL %s, device %d Hz %d channels format %X, %d frame buffer; the mixer gives 48000 Hz stereo float",
         SDL_GetCurrentAudioDriver(), dev.freq, dev.channels, unsigned(dev.format), dev_frames);
    if (dev.freq > 0 && dev_frames > 0) g_period_ms = 1000.0 * dev_frames / dev.freq;
    SDL_ResumeAudioStreamDevice(stream);
    // late callbacks in the log every 30 s when there were any (the device may have played silence)
    std::thread([] {
        uint64_t last = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (g_late != last)
                LOGI("audio: %llu late device callbacks of %llu so far", (unsigned long long)g_late.load(),
                     (unsigned long long)g_callbacks.load());
            last = g_late;
        }
    }).detach();
}

}  // namespace audio
