#pragma once

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <thread>

namespace skyframe {

class FramePacer {
public:
    FramePacer();

    void Reset();

    // Sets target refresh rate from config or system (e.g. 60, 90, 40, etc.)
    void SetTargetHz(int hz);

    // Call at the start of vkQueuePresentKHR to pace the base game
    // Enforces that base presents happen at cadence T_base = 2 / H
    void PaceBasePresent();

    // Checks if an intermediate frame should be inserted
    bool ShouldGenerateIntermediate();

    // Calculates current game base FPS
    float GetBaseFps() const { return m_baseFps; }
    
    // Calculates total output FPS (including generated frames)
    float GetOutputFps() const { return m_outputFps; }

    // Nanoseconds between recent game frames
    uint64_t GetAverageFrameTimeNs() const { return m_avgFrameTimeNs; }

    // Target pacing delay for intermediate frame (half the base frame time or display VBlank)
    uint64_t GetTargetPacingDelayNs() const { return m_stepDurationNs; }

    int GetTargetHz() const { return m_targetHz; }

private:
    using Clock = std::chrono::steady_clock;

    int m_targetHz = 60;
    uint64_t m_stepDurationNs = 16666666; // 10^9 / 60
    uint64_t m_baseDurationNs = 33333333; // 2 * stepDuration

    Clock::time_point m_lastBasePresentTime;
    Clock::time_point m_nextAllowedBaseTime;

    uint64_t m_avgFrameTimeNs = 33333333; // Default 30 FPS (~33.3ms)
    float m_baseFps = 30.0f;
    float m_outputFps = 60.0f;
    uint64_t m_frameCount = 0;
    bool m_firstFrame = true;

    void RecalculateDurations();
    void HighPrecisionSleepUntil(Clock::time_point targetTime);
};

} // namespace skyframe
