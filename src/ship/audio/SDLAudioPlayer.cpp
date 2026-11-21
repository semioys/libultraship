#include "ship/audio/SDLAudioPlayer.h"
#include <spdlog/spdlog.h>
#ifdef __SWITCH__
#include "ship/port/switch/SwitchImpl.h"
#endif

namespace Ship {

SDLAudioPlayer::~SDLAudioPlayer() {
    SPDLOG_TRACE("destruct SDL audio player");
    DoClose();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void SDLAudioPlayer::DoClose() {
    if (mDevice != 0) {
        // Pause playback first
        SDL_PauseAudioDevice(mDevice, 1);
        // Clear any queued audio to prevent glitches when reopening
        SDL_ClearQueuedAudio(mDevice);
        SDL_CloseAudioDevice(mDevice);
        mDevice = 0;
    }
}

bool SDLAudioPlayer::DoInit() {
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        SPDLOG_ERROR("SDL init error: {}", SDL_GetError());
        return false;
    }

    // Always open with the correct number of output channels
    mNumChannels = this->GetNumOutputChannels();

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = this->GetSampleRate();
    want.format = AUDIO_S16SYS;
    want.channels = mNumChannels;
    want.samples = this->GetSampleLength();
    want.callback = NULL;

    mDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (mDevice == 0) {
        SPDLOG_ERROR("SDL_OpenAudio error: {}", SDL_GetError());
        return false;
    }

    SPDLOG_INFO("SDL Audio initialized: {} channels, {} Hz", mNumChannels, this->GetSampleRate());

    SDL_PauseAudioDevice(mDevice, 0);
    return true;
}

int SDLAudioPlayer::Buffered() {
    return SDL_GetQueuedAudioSize(mDevice) / (sizeof(int16_t) * mNumChannels);
}

void SDLAudioPlayer::DoPlay(const uint8_t* buf, size_t len) {
#ifdef __SWITCH__
    // HOME menu / overlays: stop output instead of looping the last buffer.
    // After sleep the audio renderer is gone, so reopen the device.
    static uint32_t sSeenResumeCount = 0;
    static bool sPaused = false;
    const uint32_t resumeCount = Ship::Switch::GetResumeCount();
    if (resumeCount != sSeenResumeCount) {
        sSeenResumeCount = resumeCount;
        DoClose();
        const bool reopened = DoInit();
        sPaused = false;
        Ship::Switch::Diag("audio device reopened after sleep (%s)", reopened ? "ok" : "FAILED");
    }
    const bool focused = Ship::Switch::IsInFocus();
    if (!focused && !sPaused && mDevice != 0) {
        SDL_PauseAudioDevice(mDevice, 1);
        SDL_ClearQueuedAudio(mDevice);
        sPaused = true;
    } else if (focused && sPaused && mDevice != 0) {
        SDL_PauseAudioDevice(mDevice, 0);
        sPaused = false;
    }
    if (sPaused || mDevice == 0) {
        return;
    }
#endif
    if (Buffered() < 6000) {
        // Don't fill the audio buffer too much in case this happens
        SDL_QueueAudio(mDevice, buf, len);
    }
}
} // namespace Ship
