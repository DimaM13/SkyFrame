#include "frame_pacer.h"

namespace skyframe {

FramePacer::FramePacer() {
    m_lastPresentTime = Clock::now();
}

void FramePacer::OnGamePresent() {
    auto now = Clock::now();
    auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_lastPresentTime).count();
    m_lastPresentTime = now;

    if (elapsedNs > 1000000 && elapsedNs < 500000000) { // 2 FPS to 1000 FPS valid range
        // Exponential moving average to smooth pacing
        m_avgFrameTimeNs = static_cast<uint64_t>(m_avgFrameTimeNs * 0.9 + elapsedNs * 0.1);
        m_baseFps = 1000000000.0f / static_cast<float>(m_avgFrameTimeNs);
        m_outputFps = m_baseFps * 2.0f;
    }
    m_frameCount++;
}

bool FramePacer::ShouldGenerateIntermediate() {
    // Only generate if base FPS is reasonably stable (>= 15 FPS)
    return (m_baseFps >= 15.0f && m_baseFps <= 120.0f);
}

} // namespace skyframe
