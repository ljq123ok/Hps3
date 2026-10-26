#pragma once
#include "Emu/Audio/AudioBackend.h"
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiorenderer.h>
#include <dlfcn.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <mutex>

inline std::atomic<uint64_t> hps3_audio_callbacks{0}, hps3_audio_bytes{0}, hps3_audio_nonzero{0};

// Resolve the public OHAudio API at runtime so a missing service can be reported
// as backend initialization failure rather than preventing the core from loading.
class ohos_audio_backend final : public AudioBackend
{
#define HPS3_AUDIO_API(name) decltype(&name) p_##name = nullptr
    HPS3_AUDIO_API(OH_AudioStreamBuilder_Create);
    HPS3_AUDIO_API(OH_AudioStreamBuilder_Destroy);
    HPS3_AUDIO_API(OH_AudioStreamBuilder_SetSamplingRate);
    HPS3_AUDIO_API(OH_AudioStreamBuilder_SetChannelCount);
    HPS3_AUDIO_API(OH_AudioStreamBuilder_SetSampleFormat);
    HPS3_AUDIO_API(OH_AudioStreamBuilder_SetRendererInfo);
    HPS3_AUDIO_API(OH_AudioStreamBuilder_SetRendererCallback);
    HPS3_AUDIO_API(OH_AudioStreamBuilder_GenerateRenderer);
    HPS3_AUDIO_API(OH_AudioRenderer_Start);
    HPS3_AUDIO_API(OH_AudioRenderer_Pause);
    HPS3_AUDIO_API(OH_AudioRenderer_Release);
    HPS3_AUDIO_API(OH_AudioRenderer_GetFrameSizeInCallback);
#undef HPS3_AUDIO_API
    void* library = nullptr;
    OH_AudioRenderer* renderer = nullptr;
    std::atomic<bool> failed{false};
    bool available = false;
    int32_t callback_frames = 480;

    static int32_t write_audio(OH_AudioRenderer*, void* user, void* buffer, int32_t length)
    {
        if (length <= 0) return 0;
        auto& self = *static_cast<ohos_audio_backend*>(user);
        const auto requested = static_cast<u32>(length);
        u32 written = 0;
        {
            std::unique_lock lock(self.m_cb_mutex, std::defer_lock);
            if (lock.try_lock_for(std::chrono::microseconds{50}) && self.m_playing && self.m_write_callback)
                written = std::min(requested, self.m_write_callback(requested, buffer));
        }
        if (written < requested)
            std::memset(static_cast<u8*>(buffer) + written, 0, requested - written);
        hps3_audio_callbacks.fetch_add(1, std::memory_order_relaxed);
        hps3_audio_bytes.fetch_add(written, std::memory_order_relaxed);
        // Count buffers carrying actual guest audio, independently of callbacks.
        const auto* bytes = static_cast<const u8*>(buffer);
        for (u32 i = 0; i < written; ++i)
            if (bytes[i]) { hps3_audio_nonzero.fetch_add(1, std::memory_order_relaxed); break; }
        return 0;
    }
public:
    ohos_audio_backend()
    {
        library = dlopen("libohaudio.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) { HPS3_LOG("HPS3_AUDIO library unavailable"); return; }
        available = true;
#define HPS3_LOAD_AUDIO(name) p_##name = reinterpret_cast<decltype(p_##name)>(dlsym(library, #name)); available &= p_##name != nullptr
        HPS3_LOAD_AUDIO(OH_AudioStreamBuilder_Create);
        HPS3_LOAD_AUDIO(OH_AudioStreamBuilder_Destroy);
        HPS3_LOAD_AUDIO(OH_AudioStreamBuilder_SetSamplingRate);
        HPS3_LOAD_AUDIO(OH_AudioStreamBuilder_SetChannelCount);
        HPS3_LOAD_AUDIO(OH_AudioStreamBuilder_SetSampleFormat);
        HPS3_LOAD_AUDIO(OH_AudioStreamBuilder_SetRendererInfo);
        HPS3_LOAD_AUDIO(OH_AudioStreamBuilder_SetRendererCallback);
        HPS3_LOAD_AUDIO(OH_AudioStreamBuilder_GenerateRenderer);
        HPS3_LOAD_AUDIO(OH_AudioRenderer_Start);
        HPS3_LOAD_AUDIO(OH_AudioRenderer_Pause);
        HPS3_LOAD_AUDIO(OH_AudioRenderer_Release);
        HPS3_LOAD_AUDIO(OH_AudioRenderer_GetFrameSizeInCallback);
#undef HPS3_LOAD_AUDIO
        HPS3_LOG("HPS3_AUDIO API ready=%{public}d", available ? 1 : 0);
    }
    ~ohos_audio_backend() override { Close(); if (library) dlclose(library); }
    std::string_view GetName() const override { return "OHAudio"; }
    bool Initialized() override { return available; }
    bool Operational() override { return available && renderer && !failed.load(); }
    bool Open(std::string_view, AudioFreq freq, AudioSampleSize size, AudioChannelCnt channels, audio_channel_layout) override
    {
        Close();
        if (!available) return false;
        m_sampling_rate = freq;
        m_sample_size = size;
        m_channels = std::min<u32>(static_cast<u32>(channels), 2);
        m_layout = default_layout(m_channels);
        failed.store(false);
        OH_AudioStreamBuilder* builder = nullptr;
        if (p_OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS)
            return false;
        OH_AudioRenderer_Callbacks callbacks{};
        callbacks.OH_AudioRenderer_OnWriteData = write_audio;
        callbacks.OH_AudioRenderer_OnError = [](OH_AudioRenderer*, void* user, OH_AudioStream_Result) -> int32_t
        { static_cast<ohos_audio_backend*>(user)->failed.store(true); return 0; };
        const bool ok = p_OH_AudioStreamBuilder_SetSamplingRate(builder, get_sampling_rate()) == AUDIOSTREAM_SUCCESS &&
            p_OH_AudioStreamBuilder_SetChannelCount(builder, m_channels) == AUDIOSTREAM_SUCCESS &&
            p_OH_AudioStreamBuilder_SetSampleFormat(builder, size == AudioSampleSize::FLOAT ? AUDIOSTREAM_SAMPLE_F32LE : AUDIOSTREAM_SAMPLE_S16LE) == AUDIOSTREAM_SUCCESS &&
            p_OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_GAME) == AUDIOSTREAM_SUCCESS &&
            p_OH_AudioStreamBuilder_SetRendererCallback(builder, callbacks, this) == AUDIOSTREAM_SUCCESS &&
            p_OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer) == AUDIOSTREAM_SUCCESS;
        p_OH_AudioStreamBuilder_Destroy(builder);
        if (!ok) { Close(); HPS3_LOG("HPS3_AUDIO open failed"); return false; }
        p_OH_AudioRenderer_GetFrameSizeInCallback(renderer, &callback_frames);
        HPS3_LOG("HPS3_AUDIO open rate=%{public}u channels=%{public}u bytes=%{public}u frames=%{public}d",
            get_sampling_rate(), m_channels, get_sample_size(), callback_frames);
        return true;
    }
    void Close() override
    {
        if (renderer) { p_OH_AudioRenderer_Release(renderer); renderer = nullptr; }
        std::lock_guard lock(m_cb_mutex);
        m_playing = false;
    }
    f64 GetCallbackFrameLen() override { return static_cast<f64>(callback_frames) / get_sampling_rate(); }
    void Play() override
    {
        if (!renderer) return;
        { std::lock_guard lock(m_cb_mutex); if (m_playing) return; m_playing = true; }
        const auto result = p_OH_AudioRenderer_Start(renderer);
        if (result != AUDIOSTREAM_SUCCESS) { std::lock_guard lock(m_cb_mutex); m_playing = false; failed.store(true); }
        HPS3_LOG("HPS3_AUDIO start result=%{public}d", static_cast<int>(result));
    }
    void Pause() override
    {
        if (renderer) p_OH_AudioRenderer_Pause(renderer);
        std::lock_guard lock(m_cb_mutex);
        m_playing = false;
    }
    bool IsPlaying() override { std::lock_guard lock(m_cb_mutex); return m_playing; }
};
