#include "safety.h"

namespace safety {

namespace {

// Square root by Newton's method: no libm call, so the code stays self-contained in IRAM.
// Used only when a calibration window completes, never per sample.
float root(float x) {
    if (x <= 0.0f) return 0.0f;
    float r = x > 1.0f ? x : 1.0f;
    for (int i = 0; i < 30; ++i) {
        r = 0.5f * (r + x / r);
    }
    return r;
}

float absf(float v) { return v < 0.0f ? -v : v; }

}  // namespace

void RestDetector::restart() {
    if (n_ > 0) cal_restarts_++;
    n_ = 0;
    a2_mean_ = 0;
    a2_m2_ = 0;
    for (int i = 0; i < 3; ++i) {
        mean_[i] = 0;
        m2_[i] = 0;
    }
}

void RestDetector::reset() {
    const uint32_t restarts = cal_restarts_;
    *this = RestDetector(cfg_);
    cal_restarts_ = restarts;
}

void RestDetector::sample(int64_t now_us, const float g[3], const float a[3]) {
    const float a2 = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];

    if (!cal_done_) {
        // Calibration window: variation only (Welford), never the absolute bias, except the
        // loose sanity bound applied at the end.
        n_++;
        bool moved = false;
        for (int i = 0; i < 3; ++i) {
            if (n_ == 1) {
                min_[i] = max_[i] = g[i];
            } else {
                if (g[i] < min_[i]) min_[i] = g[i];
                if (g[i] > max_[i]) max_[i] = g[i];
            }
            const float d = g[i] - mean_[i];
            mean_[i] += d / static_cast<float>(n_);
            m2_[i] += d * (g[i] - mean_[i]);
            moved = moved || (max_[i] - min_[i]) > cfg_.cal_range_max_dps;
        }
        const float da = a2 - a2_mean_;
        a2_mean_ += da / static_cast<float>(n_);
        a2_m2_ += da * (a2 - a2_mean_);
        if (moved) {
            restart();  // motion during the window: start again at once
            return;
        }
        if (n_ < cfg_.cal_samples) return;

        bool ok = true;
        float var_max = 0;
        const float sd2 = cfg_.cal_sd_max_dps * cfg_.cal_sd_max_dps;
        for (int i = 0; i < 3; ++i) {
            const float var = m2_[i] / static_cast<float>(n_ - 1);
            if (var > var_max) var_max = var;
            ok = ok && var <= sd2 && absf(mean_[i]) <= cfg_.cal_bias_max_dps;
        }
        // Accelerometer norm: sd(|a|^2) ~ 2 g0 sd(|a|), so var(|a|^2) <= 4 g0^2 sd_max^2.
        const float a2_var = a2_m2_ / static_cast<float>(n_ - 1);
        ok = ok && a2_var <= 4.0f * a2_mean_ * cfg_.cal_norm_sd_max_g * cfg_.cal_norm_sd_max_g;
        if (!ok) {
            restart();
            return;
        }
        for (int i = 0; i < 3; ++i) bias_[i] = mean_[i];
        g0_sq_ = a2_mean_;
        g0_ = root(a2_mean_);
        ref_ok_ = g0_ >= cfg_.ref_min_g && g0_ <= cfg_.ref_max_g;
        cal_sd_max_ = root(var_max);
        cal_done_ = true;
        rest_since_us_ = now_us - cfg_.rest_hold_us;  // the window itself proved rest
        block_n_ = 0;
        block_gyro_dev_ = 0;
        block_accel_out_ = false;
    }

    // Accelerometer validity and tilt, relative to g0 (R6), from squared magnitudes.
    const float lo = cfg_.valid_min_frac * cfg_.valid_min_frac * g0_sq_;
    const float hi = cfg_.valid_max_frac * cfg_.valid_max_frac * g0_sq_;
    accel_ok_ = ref_ok_ && a2 >= lo && a2 <= hi;
    tilt_ok_ = accel_ok_ && a[2] > 0.0f && a[2] * a[2] >= cfg_.cos_tilt_limit * cfg_.cos_tilt_limit * a2;

    // Motion, judged per block.
    for (int i = 0; i < 3; ++i) {
        const float dev = absf(g[i] - bias_[i]);
        if (dev > block_gyro_dev_) block_gyro_dev_ = dev;
    }
    const float m_lo = (1.0f - cfg_.motion_accel_frac) * (1.0f - cfg_.motion_accel_frac) * g0_sq_;
    const float m_hi = (1.0f + cfg_.motion_accel_frac) * (1.0f + cfg_.motion_accel_frac) * g0_sq_;
    if (a2 < m_lo || a2 > m_hi) block_accel_out_ = true;
    if (++block_n_ >= cfg_.block_samples) {
        if (block_gyro_dev_ > cfg_.motion_gyro_dps || block_accel_out_) {
            rest_since_us_ = now_us;
        }
        block_n_ = 0;
        block_gyro_dev_ = 0;
        block_accel_out_ = false;
    }
}

bool RestDetector::at_rest(int64_t now_us) const {
    return cal_done_ && now_us - rest_since_us_ >= cfg_.rest_hold_us;
}

}  // namespace safety
