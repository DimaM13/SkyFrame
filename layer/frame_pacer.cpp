#include "frame_pacer.h"

namespace skyframe {

FramePacer::FramePacer() {
    Reset();
}

void FramePacer::Reset() {
    m_lastPresentTime = Clock::now();
    m_avgFrameTimeNs = 33333333;
    m_baseFps = 30.0f;
    m_outputFps = 60.0f;
    m_frameCount = 0;
    m_firstFrame = true;
}

void FramePacer::OnGamePresent() {
    auto now = Clock::now();
    if (m_firstFrame) {
        m_lastPresentTime = now;
        m_firstFrame = false;
        m_frameCount++;
        return;
    }

    auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_lastPresentTime).count();
    m_lastPresentTime = now;

    // Filter out pause/loading spikes (> 250ms) and microbursts (< 2ms)
    if (elapsedNs >= 2000000 && elapsedNs <= 250000000) {
        // Adaptive EMA: 80% history, 20% new sample
        m_avgFrameTimeNs = static_cast<uint64_t>(m_avgFrameTimeNs * 0.80 + elapsedNs * 0.20);
        m_baseFps = 1000000000.0f / static_cast<float>(m_avgFrameTimeNs);
        m_outputFps = m_baseFps * 2.0f;
    }
    m_frameCount++;
}

bool FramePacer::ShouldGenerateIntermediate() {
    // Only generate if base FPS is reasonably stable (>= 15 FPS and <= 120 FPS)
    return (m_baseFps >= 15.0f && m_baseFps <= 120.0f);
}

} // namespace skyframe
