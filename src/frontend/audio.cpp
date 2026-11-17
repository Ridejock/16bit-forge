// SPDX-License-Identifier: MIT
#include "frontend/audio.hpp"
#include "utils/logging.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace bitforge::frontend {

namespace {

constexpr int kChannels = 2;
constexpr int kBytesPerFrame = kChannels * static_cast<int>(sizeof(int16_t));
constexpr int kUnityGain = 1 << 15;  // Q15
constexpr int kMaxVolume = 100;
constexpr double kFifoSeconds = 1.0;  // capacity; kMaxQueueMs keeps it far from full

auto ms_to_frames(double ms, int rate) -> size_t { return static_cast<size_t>(ms * rate / 1000.0); }

auto to_int16(float f) -> int16_t {
    const float s = std::clamp(f, -1.0f, 1.0f) * 32767.0f;
    return static_cast<int16_t>(std::lrintf(s));
}

} // namespace

// ---- rate control ----------------------------------------------------------------------------

auto RateControl::restart() -> void {
    level_ms_ = target_ms_;
    ratio_ = 1.0 + integral_;  // keep what was learned about the clocks across pauses
}

auto RateControl::update(double queued_ms) -> double {
    // The device drains the queue in chunks, so one reading jumps around; steer on an average.
    level_ms_ += (queued_ms - level_ms_) * kLevelSmoothing;
    const double error = std::clamp((level_ms_ - target_ms_) / target_ms_, -1.0, 1.0);
    // More queued -> consume a little faster (ratio > 1). Anti-windup: the integral alone can
    // never exceed the clamp.
    integral_ = std::clamp(integral_ + kIntegral * error, -kMaxRateAdjust, kMaxRateAdjust);
    ratio_ = 1.0 + std::clamp(kProportional * error + integral_, -kMaxRateAdjust, kMaxRateAdjust);
    return ratio_;
}

// The queue's low point is roughly target - one device pull - one frame of jitter; keep it
// clear of zero when the device pulls big chunks (some USB/Bluetooth devices: 20-50 ms).
auto AudioOutput::target_ms_for(int pull_frames) const -> double {
    const double pull_ms = source_rate_ > 0 ? 1000.0 * pull_frames / source_rate_ : 0.0;
    return std::max(kTargetMs, 1.25 * pull_ms + kFrameMs);
}

// After an underrun (or at start), wait for 3/4 of the target before playing again.
auto AudioOutput::resume_frames(double target_ms) const -> size_t {
    return ms_to_frames(0.75 * target_ms, source_rate_);
}

// ---- device ----------------------------------------------------------------------------------

auto AudioOutput::open(int source_rate) -> bool {
    close();
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        utils::Logger::warning(utils::LogChannel::Audio, "Audio disabled: %s", SDL_GetError());
        return false;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, kChannels, source_rate};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &AudioOutput::on_stream_get, this);
    if (!stream) {
        utils::Logger::warning(utils::LogChannel::Audio, "Audio disabled: no playback device (%s)", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    owns_subsystem_ = true;
    device_ = SDL_GetAudioStreamDevice(stream);
    SDL_AudioSpec device{};
    int device_frames = 0;
    int device_rate = source_rate;
    if (SDL_GetAudioDeviceFormat(device_, &device, &device_frames)) {
        device_rate = device.freq;
        device_channels_ = device.channels;
        device_buffer_frames_ = device_frames;
    }
    open_stream(stream, source_rate, device_rate);
    if (device_rate_ > 0) {  // a first guess at the pull size until SDL actually asks
        const auto pull = static_cast<int>(static_cast<int64_t>(device_buffer_frames_) * source_rate_ / device_rate_);
        rate_.set_target(target_ms_for(pull));
    }
    utils::Logger::info(utils::LogChannel::Audio, "Audio: %d Hz stereo -> %s, %d Hz, %d ch, device buffer %d frames (%.1f ms); queue target %.0f ms",
                        source_rate_, SDL_GetCurrentAudioDriver(), device_rate_, device_channels_, device_buffer_frames_,
                        device_rate_ > 0 ? 1000.0 * device_buffer_frames_ / device_rate_ : 0.0, rate_.target_ms());
    return true;
}

auto AudioOutput::open_unbound(int source_rate, int device_rate) -> bool {
    close();
    const SDL_AudioSpec src{SDL_AUDIO_S16, kChannels, source_rate};
    const SDL_AudioSpec dst{SDL_AUDIO_S16, kChannels, device_rate};
    SDL_AudioStream* stream = SDL_CreateAudioStream(&src, &dst);
    if (!stream) return false;
    SDL_SetAudioStreamGetCallback(stream, &AudioOutput::on_stream_get, this);
    device_channels_ = kChannels;
    open_stream(stream, source_rate, device_rate);
    return true;
}

auto AudioOutput::open_stream(SDL_AudioStream* stream, int source_rate, int device_rate) -> void {
    source_rate_ = source_rate;
    device_rate_ = device_rate;
    {
        std::lock_guard lock(fifo_mutex_);
        fifo_.assign(static_cast<size_t>(kFifoSeconds * source_rate) * kChannels, 0);
        fifo_head_ = fifo_count_ = 0;
        feed_state_ = Feed::Refill;
        last_pull_frames_ = max_pull_frames_ = 0;
        priming_ = running_ = false;
        grace_frames_ = warmup_frames_ = 0;
    }
    rate_ = RateControl{};
    stream_ = stream;
    paused_ = true;
}

auto AudioOutput::close() -> void {
    if (!stream_) return;
    stop_device_recording();
    stop_core_recording();
    SDL_DestroyAudioStream(stream_);  // also closes the device it opened; no callbacks after this
    stream_ = nullptr;
    device_ = 0;
    if (owns_subsystem_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    owns_subsystem_ = false;
}

auto AudioOutput::set_paused(bool paused) -> void {
    if (!stream_ || paused == paused_) return;
    paused_ = paused;
    if (paused) {
        // Stop counting first (the audio thread checks running_ under the lock), then the device.
        {
            std::lock_guard lock(fifo_mutex_);
            running_ = false;
        }
        if (device_) SDL_PauseAudioStreamDevice(stream_);  // outside fifo_mutex_ (callback lock order)
        SDL_ClearAudioStream(stream_);
        std::lock_guard lock(fifo_mutex_);
        fifo_head_ = fifo_count_ = 0;
        feed_state_ = Feed::Refill;
        priming_ = false;
        grace_frames_ = warmup_frames_ = 0;
        pending_underruns_ = pending_silence_frames_ = 0;  // never confirmed by another frame
        pending_starved_frames_ = pulled_since_push_ = 0;
    } else {
        // Don't start the device yet: push() starts it once kWarmupFrames have come in and the
        // FIFO holds the target. Until then, and for kGraceFrames pushes after, nothing counts.
        rate_.restart();
        std::lock_guard lock(fifo_mutex_);
        priming_ = true;
        running_ = false;
        feed_state_ = Feed::Refill;
        warmup_frames_ = kWarmupFrames;
        grace_frames_ = kGraceFrames;
    }
}

auto AudioOutput::hold() -> void {
    {
        std::lock_guard lock(fifo_mutex_);
        running_ = false;
    }
    if (device_) SDL_PauseAudioStreamDevice(stream_);
    SDL_ClearAudioStream(stream_);
    rate_.restart();
    std::lock_guard lock(fifo_mutex_);
    fifo_head_ = fifo_count_ = 0;
    feed_state_ = Feed::Refill;
    priming_ = true;
    warmup_frames_ = kWarmupFrames;
    grace_frames_ = kGraceFrames;
    pending_underruns_ = pending_silence_frames_ = 0;
    pending_starved_frames_ = pulled_since_push_ = 0;
}

auto AudioOutput::begin_session() -> void {
    {
        std::lock_guard lock(fifo_mutex_);
        underruns_ = silence_frames_ = overflows_ = dropped_frames_ = 0;
        pending_underruns_ = pending_silence_frames_ = 0;
        pending_starved_frames_ = starved_frames_ = 0;
        grace_frames_ = kGraceFrames;
    }
    // Normally the output is already paused (end_session() before the ROM load). If it's still
    // running, the device has been pulling through the load: hold it and warm up again.
    if (stream_ && !paused_) hold();
}

auto AudioOutput::start_grace() -> void {
    std::lock_guard lock(fifo_mutex_);
    grace_frames_ = kGraceFrames;
}

auto AudioOutput::set_rate_control(bool on) -> void {
    rate_control_ = on;
    if (!on && stream_) SDL_SetAudioStreamFrequencyRatio(stream_, 1.0f);
}

auto AudioOutput::queued_ms() const -> double {
    if (!stream_ || source_rate_ <= 0) return 0.0;
    // Everything not yet handed to the device: our FIFO plus what the SDL stream still holds
    // (both in source frames). Measured in time at the source rate, which is what it plays as.
    const int sdl_bytes = std::max(0, SDL_GetAudioStreamQueued(stream_));  // input-format bytes
    size_t frames = static_cast<size_t>(sdl_bytes / kBytesPerFrame);
    {
        std::lock_guard lock(fifo_mutex_);
        frames += fifo_count_;
    }
    return 1000.0 * static_cast<double>(frames) / source_rate_;
}

// ---- FIFO (fifo_mutex_ held) -----------------------------------------------------------------

auto AudioOutput::fifo_frames() const -> size_t { return fifo_count_; }

auto AudioOutput::fifo_write(const int16_t* stereo, size_t frames) -> void {
    const size_t capacity = fifo_.size() / kChannels;
    if (frames > capacity - fifo_count_) frames = capacity - fifo_count_;  // can't happen below kMaxQueueMs
    size_t tail = (fifo_head_ + fifo_count_) % capacity;
    for (size_t i = 0; i < frames; i++) {
        fifo_[tail * 2] = stereo[i * 2];
        fifo_[tail * 2 + 1] = stereo[i * 2 + 1];
        tail = (tail + 1) % capacity;
    }
    fifo_count_ += frames;
}

auto AudioOutput::fifo_read(int16_t* stereo, size_t frames) -> void {
    const size_t capacity = fifo_.size() / kChannels;
    for (size_t i = 0; i < frames; i++) {
        stereo[i * 2] = fifo_[fifo_head_ * 2];
        stereo[i * 2 + 1] = fifo_[fifo_head_ * 2 + 1];
        fifo_head_ = (fifo_head_ + 1) % capacity;
    }
    fifo_count_ -= frames;
}

auto AudioOutput::fifo_drop_oldest(size_t frames) -> void {
    // Crossfade the new head with the audio that would have played next, so the cut is smooth.
    const size_t capacity = fifo_.size() / kChannels;
    frames = std::min(frames, fifo_count_);
    const size_t fade = std::min<size_t>(kFadeFrames, fifo_count_ - frames);
    for (size_t i = 0; i < fade; i++) {
        const size_t old_at = (fifo_head_ + i) % capacity;
        const size_t new_at = (fifo_head_ + frames + i) % capacity;
        const double t = static_cast<double>(i + 1) / static_cast<double>(fade + 1);
        for (int c = 0; c < kChannels; c++) {
            const double v = fifo_[old_at * 2 + c] * (1.0 - t) + fifo_[new_at * 2 + c] * t;
            fifo_[new_at * 2 + c] = static_cast<int16_t>(std::lrint(v));
        }
    }
    fifo_head_ = (fifo_head_ + frames) % capacity;
    fifo_count_ -= frames;
}

// ---- main thread -----------------------------------------------------------------------------

// The APU plus the MSU-1 when there is one, Q15 gains, saturated once at the end.
auto mix_sources(std::span<const int16_t> apu, std::span<const int16_t> msu1, int volume, int msu1_volume,
                 std::vector<int16_t>& out) -> void {
    const int gain = std::clamp(volume, 0, kMaxVolume) * kUnityGain / kMaxVolume;
    const int msu1_gain = gain * std::clamp(msu1_volume, 0, kMaxVolume) / kMaxVolume;
    const size_t msu1_count = std::min(msu1.size(), apu.size());
    out.resize(apu.size());
    for (size_t i = 0; i < apu.size(); i++) {
        int64_t s = static_cast<int64_t>(apu[i]) * gain;
        if (i < msu1_count) s += static_cast<int64_t>(msu1[i]) * msu1_gain;
        out[i] = static_cast<int16_t>(std::clamp<int64_t>(s >> 15, -32768, 32767));
    }
}

auto AudioOutput::push(std::span<const int16_t> stereo, std::span<const int16_t> msu1) -> void {
    if (core_rec_.is_open()) core_rec_.write(stereo);  // raw DSP output of every emulated frame, before the mixer
    if (!stream_ || paused_ || stereo.empty()) return;

    // Rate control, measured just before the push (the low point of the queue). SDL calls are
    // made without fifo_mutex_ held: the get-callback runs under the stream lock and takes it.
    const double queued = queued_ms();
    int max_pull = 0;
    {
        std::lock_guard lock(fifo_mutex_);
        max_pull = max_pull_frames_;
    }
    if (max_pull > 0) rate_.set_target(target_ms_for(max_pull));
    if (rate_control_) SDL_SetAudioStreamFrequencyRatio(stream_, static_cast<float>(rate_.update(queued)));

    mix_sources(stereo, msu1, muted_ ? 0 : volume_, msu1_volume_, mix_);

    if (fade_in_next_) {
        fade_in_next_ = false;
        const size_t frames = std::min<size_t>(kFadeFrames, mix_.size() / kChannels);
        for (size_t i = 0; i < frames; i++) {
            const double g = static_cast<double>(i) / static_cast<double>(frames);
            mix_[i * 2] = static_cast<int16_t>(mix_[i * 2] * g);
            mix_[i * 2 + 1] = static_cast<int16_t>(mix_[i * 2 + 1] * g);
        }
    }

    bool start_device = false;
    {
        std::lock_guard lock(fifo_mutex_);
        // Emulation went on after whatever the device counted: those underruns are real.
        // A new underrun starts a new episode; its refill silence (later pushes) only extends it.
        if (pending_underruns_ > 0) starved_frames_ = pending_starved_frames_;
        else if (pending_silence_frames_ > 0) starved_frames_ = std::max(starved_frames_, pending_starved_frames_);
        underruns_ += pending_underruns_;
        silence_frames_ += pending_silence_frames_;
        pending_underruns_ = pending_silence_frames_ = 0;
        pending_starved_frames_ = pulled_since_push_ = 0;
        fifo_write(mix_.data(), mix_.size() / kChannels);
        const size_t max_frames = ms_to_frames(std::max(kMaxQueueMs, 3.0 * rate_.target_ms()), source_rate_);
        if (fifo_count_ > max_frames) {  // e.g. catching up after a stall: drop back to the target
            const size_t drop = fifo_count_ - std::min(fifo_count_, ms_to_frames(rate_.target_ms(), source_rate_));
            fifo_drop_oldest(drop);
            if (!priming_ && grace_frames_ == 0) {
                overflows_++;
                dropped_frames_ += drop;
            }
        }
        if (priming_) {
            // Held: keep the newest target's worth, so the device starts with a full (not overfull)
            // buffer. It starts once frames have been flowing for kWarmupFrames pushes, i.e. after
            // the hitches of a new game window, not on the first few frames that happen to arrive.
            const size_t target_frames = ms_to_frames(rate_.target_ms(), source_rate_);
            if (fifo_count_ > target_frames) fifo_drop_oldest(fifo_count_ - target_frames);
            if (warmup_frames_ > 0) warmup_frames_--;
            if (warmup_frames_ == 0 && fifo_count_ >= target_frames) {
                priming_ = false;
                running_ = true;
                start_device = true;
            }
        } else if (running_ && grace_frames_ > 0) {
            grace_frames_--;  // grace runs on emulated frames: a stalled emulation thread doesn't use it up
        }
    }
    if (start_device && device_) SDL_ResumeAudioStreamDevice(stream_);  // outside fifo_mutex_ (callback lock order)
}

auto AudioOutput::flush() -> void {
    if (!stream_) return;
    fade_in_next_ = true;
    std::lock_guard lock(fifo_mutex_);
    grace_frames_ = kGraceFrames;
    const size_t capacity = fifo_.size() / kChannels;
    const size_t fade = std::min<size_t>(kFadeFrames, fifo_count_);
    for (size_t i = 0; i < fifo_count_; i++) {
        const size_t at = (fifo_head_ + i) % capacity;
        const double g = i < fade ? static_cast<double>(fade - i - 1) / static_cast<double>(fade) : 0.0;
        fifo_[at * 2] = static_cast<int16_t>(fifo_[at * 2] * g);
        fifo_[at * 2 + 1] = static_cast<int16_t>(fifo_[at * 2 + 1] * g);
    }
}

auto AudioOutput::stats() const -> Stats {
    Stats s;
    s.open = stream_ != nullptr;
    s.paused = paused_;
    s.rate_control = rate_control_;
    s.driver = stream_ && device_ ? SDL_GetCurrentAudioDriver() : "";
    if (!s.driver) s.driver = "";
    s.source_rate = source_rate_;
    s.device_rate = device_rate_;
    s.device_channels = device_channels_;
    s.device_buffer_frames = device_buffer_frames_;
    s.target_ms = rate_.target_ms();
    s.queued_ms = queued_ms();
    s.level_ms = rate_.level_ms();
    s.ratio = rate_control_ ? rate_.ratio() : 1.0;
    s.learned = rate_.learned();
    std::lock_guard lock(fifo_mutex_);
    s.last_pull_frames = last_pull_frames_;
    s.priming = priming_;
    s.grace = grace_frames_ > 0;
    s.device_running = running_;
    s.underruns = underruns_;
    s.overflows = overflows_;
    if (source_rate_ > 0) {
        s.silence_ms = 1000.0 * static_cast<double>(silence_frames_) / source_rate_;
        s.dropped_ms = 1000.0 * static_cast<double>(dropped_frames_) / source_rate_;
        s.starved_ms = 1000.0 * static_cast<double>(starved_frames_) / source_rate_;
    }
    return s;
}

// ---- audio thread ----------------------------------------------------------------------------

void SDLCALL AudioOutput::on_stream_get(void* userdata, SDL_AudioStream* stream, int additional, int) {
    static_cast<AudioOutput*>(userdata)->feed(stream, additional);
}

auto AudioOutput::feed(SDL_AudioStream* stream, int additional_bytes) -> void {
    const auto needed = static_cast<size_t>(additional_bytes / kBytesPerFrame);
    if (needed == 0) return;
    {
        std::lock_guard lock(fifo_mutex_);
        last_pull_frames_ = static_cast<int>(needed);
        max_pull_frames_ = std::max(max_pull_frames_, last_pull_frames_);
        feed_buf_.assign(needed * kChannels, 0);
        int16_t* out = feed_buf_.data();
        const size_t have = fifo_count_;
        // Only a started, unpaused output plays or counts anything. Paused or held (a device that
        // is still pulling for a moment, or an unbound test stream) gets silence, uncounted. The
        // first kGraceFrames pushes after a start/reset/state load aren't counted either.
        const bool playing = running_ && !priming_;
        const bool uncounted = !playing || grace_frames_ > 0;
        if (playing) pulled_since_push_ += needed;

        if (!playing) {
            // nothing plays until push() has warmed up and filled the queue to the target
        } else if (feed_state_ == Feed::Refill) {
            const double target = target_ms_for(max_pull_frames_);
            if (have >= resume_frames(target) && have >= needed + kFadeFrames) {
                feed_state_ = Feed::Playing;
                fifo_read(out, needed);
                const size_t fade = std::min<size_t>(kFadeFrames, needed);  // fade back in
                for (size_t i = 0; i < fade; i++) {
                    const double g = static_cast<double>(i) / static_cast<double>(fade);
                    out[i * 2] = static_cast<int16_t>(out[i * 2] * g);
                    out[i * 2 + 1] = static_cast<int16_t>(out[i * 2 + 1] * g);
                }
            } else if (!uncounted) {
                pending_silence_frames_ += needed;  // still refilling: silence
                pending_starved_frames_ = pulled_since_push_;
            }
        } else if (have >= needed + kFadeFrames) {
            fifo_read(out, needed);
        } else {
            // Underrun: play what is left, faded to zero, then silence until the FIFO refills.
            // Keeping kFadeFrames in reserve means the fade always has real audio to work with.
            const size_t take = std::min(have, needed);
            fifo_read(out, take);
            fifo_drop_oldest(fifo_count_);  // a few frames short of a fade: discard
            const size_t fade = std::min<size_t>(kFadeFrames, take);
            for (size_t i = 0; i < fade; i++) {
                const size_t at = take - fade + i;
                const double g = static_cast<double>(fade - i - 1) / static_cast<double>(fade);
                out[at * 2] = static_cast<int16_t>(out[at * 2] * g);
                out[at * 2 + 1] = static_cast<int16_t>(out[at * 2 + 1] * g);
            }
            if (!uncounted) {
                pending_silence_frames_ += needed - take;
                pending_underruns_++;
                pending_starved_frames_ = pulled_since_push_;
            }
            feed_state_ = Feed::Refill;
        }
    }
    SDL_PutAudioStreamData(stream, feed_buf_.data(), static_cast<int>(feed_buf_.size() * sizeof(int16_t)));
}

// ---- recorders -------------------------------------------------------------------------------

auto AudioOutput::start_core_recording(const std::string& path) -> bool {
    if (source_rate_ <= 0) return false;
    return core_rec_.open(path, source_rate_);
}

auto AudioOutput::stop_core_recording() -> void { core_rec_.close(); }

auto AudioOutput::start_device_recording(const std::string& path) -> bool {
    if (!stream_ || !device_) return false;
    stop_device_recording();
    {
        std::lock_guard lock(capture_mutex_);
        capture_.clear();
        capture_rate_ = 0;
    }
    device_rec_path_ = path;
    device_rec_on_ = SDL_SetAudioPostmixCallback(device_, &AudioOutput::on_postmix, this);
    if (!device_rec_on_) utils::Logger::warning(utils::LogChannel::Audio, "Audio: device recording unavailable (%s)", SDL_GetError());
    return device_rec_on_;
}

auto AudioOutput::stop_device_recording() -> void {
    if (!device_rec_on_) return;
    if (device_) SDL_SetAudioPostmixCallback(device_, nullptr, nullptr);  // waits for a running callback
    device_rec_on_ = false;
    service();  // write what was captured
    device_rec_.close();
}

void SDLCALL AudioOutput::on_postmix(void* userdata, const SDL_AudioSpec* spec, float* buffer, int buflen) {
    auto* self = static_cast<AudioOutput*>(userdata);
    const int channels = std::max(1, spec->channels);
    const int frames = buflen / static_cast<int>(sizeof(float)) / channels;
    std::lock_guard lock(self->capture_mutex_);
    self->capture_rate_ = spec->freq;
    for (int i = 0; i < frames; i++) {  // first two channels (front L/R); mono is duplicated
        const float* f = buffer + static_cast<ptrdiff_t>(i) * channels;
        self->capture_.push_back(to_int16(f[0]));
        self->capture_.push_back(to_int16(channels > 1 ? f[1] : f[0]));
    }
}

auto AudioOutput::service() -> void {
    std::vector<int16_t> captured;
    int rate = 0;
    {
        std::lock_guard lock(capture_mutex_);
        captured.swap(capture_);
        rate = capture_rate_;
    }
    if (!captured.empty() && rate > 0) {
        if (!device_rec_.is_open() && !device_rec_.open(device_rec_path_, rate)) {
            utils::Logger::warning(utils::LogChannel::Audio, "Audio: could not write %s", device_rec_path_.c_str());
            if (device_) SDL_SetAudioPostmixCallback(device_, nullptr, nullptr);
            device_rec_on_ = false;
            return;
        }
        device_rec_.write(captured);
    }
    // Keep the files valid on disk about once a second.
    static uint64_t last_flush = 0;
    const uint64_t now = SDL_GetTicks();
    if (now - last_flush >= 1000) {
        last_flush = now;
        core_rec_.flush();
        device_rec_.flush();
    }
}

} // namespace bitforge::frontend
