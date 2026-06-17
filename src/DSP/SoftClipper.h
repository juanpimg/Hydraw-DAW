#pragma once
#include <cmath>
#include <algorithm>

struct SoftClipper {
    bool enabled{false};
    float drive{1.0f};

    void process(float* samplesL, float* samplesR, int frames) const {
        if (!enabled) return;
        float d = std::max(0.01f, drive);
        float invDrive = 1.0f / tanhf(d);
        for (int i = 0; i < frames; ++i) {
            samplesL[i] = tanhf(samplesL[i] * d) * invDrive;
            samplesR[i] = tanhf(samplesR[i] * d) * invDrive;
        }
    }
};
