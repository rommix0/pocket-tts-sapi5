#pragma once

// Rate / pitch / volume post-processing of the host's 24 kHz mono PCM
// stream, using the sonic time-stretch library. Doing this on the client
// keeps rate changes instant: the model always speaks at its natural
// pace and sonic reshapes the audio on the fly.

#include <stdint.h>
#include <vector>

struct sonicStreamStruct;

class AudioPost {
public:
    AudioPost();
    ~AudioPost();

    AudioPost(const AudioPost&) = delete;
    AudioPost& operator=(const AudioPost&) = delete;

    // speed: 1.0 = natural; pitch: 1.0 = natural; volume: 1.0 = full.
    void configure(float speed, float pitch, float volume);
    void setSpeed(float speed);

    // Feed PCM in, get processed PCM out (appended to `out`).
    void process(const int16_t* samples, size_t count, std::vector<int16_t>& out);
    void finish(std::vector<int16_t>& out);

private:
    bool passthrough() const;
    void drain(std::vector<int16_t>& out);

    sonicStreamStruct* stream_;
    float speed_;
    float pitch_;
    float volume_;
};
