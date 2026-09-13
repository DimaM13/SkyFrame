#include "frame_pacer.h"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace skyframe {

FramePacer::FramePacer() {
    Reset();
}

void FramePacer::Reset() {
    m_lastBasePresentTime = Clock::now();
    m_nextAllowedBaseTime = m_lastBasePresentTime;
    m_avgFrameTimeNs = 33333333;
    m_baseFps = 30.0f;
    m_outputFps = 60.0f;
    m_frameCount = 0;
    m_firstFrame = true;
    RecalculateDurations();
}

void FramePacer::SetTargetHz(int hz) {
    if (hz >= 30 && hz <= 240 && hz != m_targetHz) {
        m_targetHz = hz;
        RecalculateDurations();
    }
}

void FramePacer::RecalculateDurations() {
    int hz = (m_targetHz >= 30 && m_targetHz <= 240) ? m_targetHz : 60;
    m_stepDurationNs = 1000000000ULL / static_cast<uint64_t>(hz);
    m_baseDurationNs = m_stepDurationNs * 2ULL;
}

void FramePacer::HighPrecisionSleepUntil(Clock::time_point targetTime) {
    auto now = Clock::now();
    if (now >= targetTime) return;

    auto remaining = targetTime - now;
    // Sleep for the bulk of remaining time if > 1.5ms to save APU power
    if (remaining > std::chrono::microseconds(1500)) {
        std::this_thread::sleep_for(remaining - std::chrono::microseconds(1200));
    }

    // Microsecond spin-wait for exact frame timing
    while (Clock::now() < targetTime) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
        _mm_pause();
#else
        __builtin_ia32_pause();
#endif
#endif
    }
}

void FramePacer::PaceBasePresent() {
    auto now = Clock::now();

    if (m_firstFrame) {
        m_lastBasePresentTime = now;
        m_nextAllowedBaseTime = now + std::chrono::nanoseconds(m_baseDurationNs);
        m_firstFrame = false;
        m_frameCount++;
        return;
    }

    // Auto VSync Cadence:
    // If game renders faster than half the display refresh rate (e.g. 40 FPS on a 60 Hz display),
    // pace the base presentation to exactly T_base (e.g. 33.3ms = 30 FPS).
    // This guarantees that intermediate and real frames are spaced by EXACTLY 1 VBlank period (16.67ms),
    // eliminating 3:2 pulldown judder and stutter!
    if (now < m_nextAllowedBaseTime) {
        HighPrecisionSleepUntil(m_nextAllowedBaseTime);
        now = Clock::now();
    }

    auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_lastBasePresentTime).count();
    m_lastBasePresentTime = now;
    m_nextAllowedBaseTime = now + std::chrono::nanoseconds(m_baseDurationNs);

    // Update EMA frametime and FPS stats
    if (elapsedNs >= 2000000 && elapsedNs <= 250000000) {
        m_avgFrameTimeNs = static_cast<uint64_t>(m_avgFrameTimeNs * 0.80 + elapsedNs * 0.20);
        m_baseFps = 1000000000.0f / static_cast<float>(m_avgFrameTimeNs);
        m_outputFps = m_baseFps * 2.0f;
    }
    m_frameCount++;
}

bool FramePacer::ShouldGenerateIntermediate() {
    return (m_baseFps >= 15.0f && m_baseFps <= 120.0f);
}

} // namespace skyframe
