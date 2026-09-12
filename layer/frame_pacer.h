#pragma once

#include <chrono>
#include <cstdint>
#include <algorithm>

namespace skyframe {

class FramePacer {
public:
    FramePacer();

    // Call on every real game present
    void OnGamePresent();

    // Checks if an intermediate frame should be inserted
    bool ShouldGenerateIntermediate();

    // Calculates current game base FPS
    float GetBaseFps() const { return m_baseFps; }
    
    // Calculates total output FPS (including generated frames)
    float GetOutputFps() const { return m_outputFps; }

    // Nanoseconds between recent game frames
    uint64_t GetAverageFrameTimeNs() const { return m_avgFrameTimeNs; }

    // Target pacing delay for intermediate frame (half the average frame time)
    uint64_t GetTargetPacingDelayNs() const {
        uint64_t half = m_avgFrameTimeNs / 2;
        if (half < 8000000) half = 8000000;
        if (half > 40000000) half = 40000000;
        return half;
    }

    void Reset();

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_lastPresentTime;
    uint64_t m_avgFrameTimeNs = 33333333; // Default 30 FPS (~33.3ms)
    float m_baseFps = 30.0f;
    float m_outputFps = 60.0f;
    uint64_t m_frameCount = 0;
    bool m_firstFrame = true;
};

} // namespace skyframe
