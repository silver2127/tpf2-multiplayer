#pragma once

// One conservative budget for this process. The worst active outgoing peer
// governs it, so adding a healthy peer cannot mask another peer's congestion.
struct SteamRateController {
    static constexpr int Initial = 1024 * 1024;
    static constexpr int Minimum = 256 * 1024;
    static constexpr int Maximum = 16 * 1024 * 1024;
    int rate = Initial, ceiling = Maximum, good = 0;
    bool active = false, unknown = false, queued = false;
    float quality = 1.0f;

    void Sample(float remoteQuality, bool outgoing, bool backlog) {
        if (!outgoing) return;
        active = true;
        if (remoteQuality < 0 || remoteQuality > 1) unknown = true;
        else if (remoteQuality < quality) quality = remoteQuality;
        queued |= backlog;
    }
    int Evaluate() {
        int target = rate;
        if (active && quality < 0.90f) {
            // Remember a conservative ceiling after a failed probe; do not
            // repeatedly ramp back into the same congested rate this session.
            int learned = rate * 3 / 4;
            if (learned < Minimum) learned = Minimum;
            if (learned < ceiling) ceiling = learned;
            target = rate / 2;
            if (target < Minimum) target = Minimum;
            good = 0;
        } else if (active && !unknown && quality >= 0.98f && queued) {
            if (++good >= 3) {
                target = rate + rate / 4;
                if (target > ceiling) target = ceiling;
                good = 0;
            }
        } else good = 0;
        active = unknown = queued = false;
        quality = 1.0f;
        return target;
    }
};
