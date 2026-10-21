#pragma once

#include "trformats.h"

#include <SDL3_mixer/SDL_mixer.h>

#include <string>

// Plays one sound file at a time through SDL_mixer (the Creatures panel's battle cry previews). The audio device opens on the
// first play only; the file stays in memory and is decoded while playing.
class SoundPlayer {
public:
    SoundPlayer() = default;
    ~SoundPlayer() { release(); }
    SoundPlayer(const SoundPlayer&) = delete;
    SoundPlayer& operator=(const SoundPlayer&) = delete;

    // Starts `data` (a .wav / .ogg / .mp3 file) under `name`, replacing what was playing. false (with error()) if it cannot.
    bool play(const std::string& name, tr::Bytes data);
    void stop();
    bool playing() const;
    bool playing(const std::string& name) const { return playing() && name_ == name; }
    const std::string& error() const { return error_; }
    void release();

private:
    void releaseAudio();
    bool initialised_ = false;
    MIX_Mixer* mixer_ = nullptr;
    MIX_Track* track_ = nullptr;
    MIX_Audio* audio_ = nullptr;
    tr::Bytes file_;
    std::string name_;
    std::string error_;
};
