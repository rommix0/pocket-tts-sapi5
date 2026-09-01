#include "audio_post.h"

#include <algorithm>
#include <cmath>

#include "sonic.h"
#include "host_protocol.h"

AudioPost::AudioPost()
    : stream_(nullptr)
    , speed_(1.0f)
    , pitch_(1.0f)
    , volume_(1.0f)
{
}

AudioPost::~AudioPost()
{
    if (stream_) {
        sonicDestroyStream(stream_);
    }
}

bool AudioPost::passthrough() const
{
    return std::fabs(speed_ - 1.0f) < 0.01f
        && std::fabs(pitch_ - 1.0f) < 0.01f
        && std::fabs(volume_ - 1.0f) < 0.01f;
}

void AudioPost::configure(float speed, float pitch, float volume)
{
    speed_ = std::clamp(speed, 0.2f, 6.0f);
    pitch_ = std::clamp(pitch, 0.3f, 3.0f);
    volume_ = std::clamp(volume, 0.0f, 1.0f);

    if (!stream_ && !passthrough()) {
        stream_ = sonicCreateStream(POCKETTTS_SAMPLE_RATE, POCKETTTS_CHANNELS);
    }
    if (stream_) {
        sonicSetSpeed(stream_, speed_);
        sonicSetPitch(stream_, pitch_);
        sonicSetVolume(stream_, volume_);
        sonicSetQuality(stream_, 0);
    }
}

void AudioPost::setSpeed(float speed)
{
    configure(speed, pitch_, volume_);
}

void AudioPost::drain(std::vector<int16_t>& out)
{
    int available;
    while ((available = sonicSamplesAvailable(stream_)) > 0) {
        const size_t old_size = out.size();
        out.resize(old_size + static_cast<size_t>(available));
        const int read = sonicReadShortFromStream(stream_, out.data() + old_size, available);
        out.resize(old_size + static_cast<size_t>((std::max)(read, 0)));
        if (read <= 0) {
            break;
        }
    }
}

void AudioPost::process(const int16_t* samples, size_t count, std::vector<int16_t>& out)
{
    if (count == 0) {
        return;
    }
    if (passthrough() && !stream_) {
        out.insert(out.end(), samples, samples + count);
        return;
    }
    if (!stream_) {
        stream_ = sonicCreateStream(POCKETTTS_SAMPLE_RATE, POCKETTTS_CHANNELS);
        if (!stream_) {
            out.insert(out.end(), samples, samples + count);
            return;
        }
        sonicSetSpeed(stream_, speed_);
        sonicSetPitch(stream_, pitch_);
        sonicSetVolume(stream_, volume_);
    }
    sonicWriteShortToStream(stream_, const_cast<int16_t*>(samples), static_cast<int>(count));
    drain(out);
}

void AudioPost::finish(std::vector<int16_t>& out)
{
    if (stream_) {
        sonicFlushStream(stream_);
        drain(out);
    }
}
