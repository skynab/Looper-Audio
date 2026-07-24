#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace looper::engine
{
/**
    A single-channel feedback delay line. JUCE-free, so the delay/feedback maths
    are unit-tested headlessly. One sample in, the delayed sample out; the written
    sample is input + feedback * delayed.
*/
class DelayLine
{
public:
    void prepare(int maxDelaySamples)
    {
        const int size = std::max(2, maxDelaySamples + 1);
        buffer_.assign((size_t) size, 0.0f);
        writeIndex_ = 0;
    }

    void reset()
    {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        writeIndex_ = 0;
    }

    float processSample(float input, int delaySamples, float feedback) noexcept
    {
        if (buffer_.empty())
            return input;

        const int size = (int) buffer_.size();
        const int d    = std::clamp(delaySamples, 0, size - 1);

        int readIndex = writeIndex_ - d;
        if (readIndex < 0)
            readIndex += size;

        const float delayed = buffer_[(size_t) readIndex];
        buffer_[(size_t) writeIndex_] = input + feedback * delayed;
        writeIndex_ = (writeIndex_ + 1) % size;
        return delayed;
    }

private:
    std::vector<float> buffer_;
    int                writeIndex_ = 0;
};

} // namespace looper::engine
