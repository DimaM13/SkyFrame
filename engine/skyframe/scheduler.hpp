// SkyFrame delivery scheduler — portable policy, no Vulkan includes.
// SPDX-License-Identifier: GPL-3.0-or-later
// Mirrors engine/skyframe/PACING.md. Pure decision logic so it can be
// unit-tested on any host; present.cpp will call into it.
#pragma once

namespace skyframe {

// Delivery modes (decky-plugin/main.py writes these from `pacing`):
//   vsync  = Smooth  (queue 2, no deadline, FIFO)   — like lsfg-vk 2.0.0
//   smooth = Balanced(queue 1, deadline 0.8*interval, fresh-first, FIFO) — default
//   none   = Low-lag (queue 1, deadline 0.5*interval, fresh-first, MAILBOX)
struct SchedConfig {
    int max_queued = 1;        // max generated frames held ahead of display
    double deadline_ms = 8.8;  // 0 = wait (smooth); else drop gen if not ready in time
    bool fresh_first = true;   // a newly arrived real frame kills queued generated
    int max_gen_in_a_row = 3;  // hard cap of consecutive generated presents
};

struct SchedState {
    int queued = 0;            // generated frames currently held
    int gen_in_a_row = 0;      // consecutive generated presents
    long drops = 0;            // stale generated dropped (HUD)
    bool base_stalled = false; // last base interval > 1.5x expected
};

struct SchedInput {
    bool fg_enabled = true;    // live-toggle
    bool gen_ready = false;    // generated image finished in time
    bool real_arrived = false; // a newer real frame arrived while gen queued
    double base_interval_ms = 16.6;     // measured interval between real frames
    double expected_base_ms = 16.6;     // target_hz-derived divisor interval
};

enum class SchedAction : unsigned char {
    PresentReal = 0,      // show the real frame (also resets gen_in_a_row)
    PresentGenerated = 1, // show one generated frame
    DropStale = 2         // drop queued generated, show real instead
};

inline SchedAction decide(const SchedConfig& cfg, SchedState& st, const SchedInput& in) {
    if (!in.fg_enabled) {
        st.queued = 0;
        st.gen_in_a_row = 0;
        return SchedAction::PresentReal;
    }
    // Base stall: never paper over a hitch with stale frames (fresh-first core).
    if (in.base_interval_ms > 1.5 * in.expected_base_ms) {
        st.base_stalled = true;
        if (st.queued > 0) {
            st.drops += st.queued;
            st.queued = 0;
        }
        st.gen_in_a_row = 0;
        return SchedAction::PresentReal;
    }
    st.base_stalled = false;
    // A newer real invalidates queued generated work.
    if (cfg.fresh_first && in.real_arrived && st.queued > 0) {
        st.drops += st.queued;
        st.queued = 0;
        st.gen_in_a_row = 0;
        return SchedAction::DropStale;
    }
    // Deadline: late generated frame is worse than a fresh real (lag spike cut).
    if (cfg.deadline_ms > 0.0 && !in.gen_ready) {
        // Caller passes gen_ready=false when now-real_time > deadline.
        if (st.queued > 0) {
            st.drops += st.queued;
            st.queued = 0;
        }
        st.gen_in_a_row = 0;
        return SchedAction::PresentReal;
    }
    // Queue depth guard: never hold more than max_queued ahead of display.
    if (st.queued >= cfg.max_queued) {
        st.drops += 1;
        st.queued = cfg.max_queued > 0 ? cfg.max_queued - 1 : 0;
        st.gen_in_a_row = 0;
        return SchedAction::DropStale;
    }
    // Consecutive-generated cap (anti-smear on sustained base drops).
    if (st.gen_in_a_row >= cfg.max_gen_in_a_row) {
        st.gen_in_a_row = 0;
        return SchedAction::PresentReal;
    }
    if (in.gen_ready) {
        st.gen_in_a_row += 1;
        st.queued = 0;
        return SchedAction::PresentGenerated;
    }
    st.gen_in_a_row = 0;
    return SchedAction::PresentReal;
}

// Vblank phasing helper: ms until the next vblank boundary.
inline double ms_until_vblank(double now_ms, double interval_ms) {
    if (interval_ms <= 0.0) return 0.0;
    double phase = now_ms - (long)(now_ms / interval_ms) * interval_ms;
    double left = interval_ms - phase;
    return left >= interval_ms ? 0.0 : left;
}

} // namespace skyframe
