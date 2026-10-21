#include "SoundPlayer.h"

bool SoundPlayer::play(const std::string& name, tr::Bytes data) {
    stop();
    releaseAudio();
    error_.clear();
    if (!initialised_) initialised_ = MIX_Init();
    if (!initialised_) {
        error_ = std::string("SDL_mixer: ") + SDL_GetError();
        return false;
    }
    if (!mixer_) mixer_ = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (mixer_ && !track_) track_ = MIX_CreateTrack(mixer_);
    if (!track_) {
        error_ = std::string("no audio device: ") + SDL_GetError();
        return false;
    }
    file_ = std::move(data);
    SDL_IOStream* io = SDL_IOFromConstMem(file_.data(), file_.size());
    audio_ = io ? MIX_LoadAudio_IO(mixer_, io, false, true) : nullptr;
    if (!audio_ || !MIX_SetTrackAudio(track_, audio_) || !MIX_SetTrackPlaybackPosition(track_, 0) || !MIX_PlayTrack(track_, 0)) {
        error_ = name + ": " + SDL_GetError();
        releaseAudio();
        return false;
    }
    name_ = name;
    return true;
}

void SoundPlayer::stop() {
    if (track_) MIX_StopTrack(track_, 0);
}

bool SoundPlayer::playing() const { return track_ && audio_ && MIX_TrackPlaying(track_); }

void SoundPlayer::releaseAudio() {
    if (track_) MIX_SetTrackAudio(track_, nullptr);
    if (audio_) MIX_DestroyAudio(audio_);
    audio_ = nullptr;
    file_.clear();
    name_.clear();
}

void SoundPlayer::release() {
    stop();
    releaseAudio();
    if (mixer_) MIX_DestroyMixer(mixer_);       // destroys its tracks too
    mixer_ = nullptr;
    track_ = nullptr;
    if (initialised_) MIX_Quit();
    initialised_ = false;
}
