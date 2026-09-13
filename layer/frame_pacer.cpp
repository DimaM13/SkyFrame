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
    m_nextBaseAt.reset();
    m_activeBaseFps = 0.0;
    m_avgFrameTimeNs = 33333333;
    m_baseFps = 30.0f;
    m_outputFps = 60.0f;
    m_frameCount = 0;
    RecalculateDurations();
}

void FramePacer::SetTargetHz(int hz) {
    if (hz >= 30 && hz <= 240 && hz != m_targetHz) {
        m_targetHz = hz;
        RecalculateDurations();
        m_nextBaseAt.reset();
    }
}

void FramePacer::RecalculateDurations() {
    int hz = (m_targetHz >= 30 && m_targetHz <= 240) ? m_targetHz : 60;
    m_stepDurationNs = 1000000000ULL / static_cast<uint64_t>(hz);
    m_baseDurationNs = m_stepDurationNs * 2ULL;
}

FramePacer::TimePoint FramePacer::Schedule(TimePoint now, double baseFps) {
    if (!std::isfinite(baseFps) || baseFps <= 0.0) {
        m_nextBaseAt.reset();
        m_activeBaseFps = 0.0;
        return now;
    }

    if (m_activeBaseFps != baseFps) {
        m_nextBaseAt.reset();
        m_activeBaseFps = baseFps;
    }

    const auto interval = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / baseFps)
    );

    if (!m_nextBaseAt) {
        m_nextBaseAt = now + interval;
        return now;
    }

    // Late frame rebases immediately so a loading stall or lag spike cannot create a burst of catch-up presents
    if (now >= *m_nextBaseAt) {
        m_nextBaseAt = now + interval;
        return now;
    }

    const auto deadline = *m_nextBaseAt;
    *m_nextBaseAt += interval;
    return deadline;
}

void FramePacer::HighPrecisionSleepUntil(TimePoint targetTime) {
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
    double baseFpsCap = static_cast<double>(m_targetHz) / 2.0;
    if (baseFpsCap < 15.0) baseFpsCap = 30.0;

    auto deadline = Schedule(now, baseFpsCap);
    if (deadline > now) {
        HighPrecisionSleepUntil(deadline);
        now = Clock::now();
    }

    auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_lastBasePresentTime).count();
    m_lastBasePresentTime = now;

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
