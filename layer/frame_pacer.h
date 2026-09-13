#pragma once

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <thread>
#include <optional>
#include <cmath>

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

    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void HighPrecisionSleepUntil(TimePoint targetTime);

private:
    int m_targetHz = 60;
    uint64_t m_stepDurationNs = 16666666; // 10^9 / 60
    uint64_t m_baseDurationNs = 33333333; // 2 * stepDuration

    double m_activeBaseFps = 0.0;
    std::optional<TimePoint> m_nextBaseAt;

    TimePoint m_lastBasePresentTime;

    uint64_t m_avgFrameTimeNs = 33333333; // Default 30 FPS (~33.3ms)
    float m_baseFps = 30.0f;
    float m_outputFps = 60.0f;
    uint64_t m_frameCount = 0;

    void RecalculateDurations();
    TimePoint Schedule(TimePoint now, double baseFps);
};

} // namespace skyframe
