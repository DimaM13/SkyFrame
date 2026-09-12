#pragma once

#include <chrono>
#include <cstdint>

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

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_lastPresentTime;
    uint64_t m_avgFrameTimeNs = 33333333; // Default 30 FPS (~33.3ms)
    float m_baseFps = 30.0f;
    float m_outputFps = 60.0f;
    uint64_t m_frameCount = 0;
};

} // namespace skyframe
