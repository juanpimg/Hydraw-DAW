#pragma once
#include "Constants.h"
#include <atomic>
#include <array>
#include <string>
#include <vector>
#include <memory>

struct AudioSample {
    std::atomic<float> left{0.0f};
    std::atomic<float> right{0.0f};
};

struct AudioBuffer {
    std::vector<float> dataL;
    std::vector<float> dataR;
    uint64_t frames{0};
    std::vector<float> peakCache;
};

struct TransportState {
    std::atomic<bool> playing{false};
    std::atomic<uint64_t> playhead{0};
    std::atomic<uint64_t> totalFrames{0};
};

struct MasterBus {
    std::atomic<float> volume{1.0f};
    std::atomic<float> pan{0.0f};
    std::atomic<float> peakL{0.0f};
    std::atomic<float> peakR{0.0f};
};

struct Track {
    std::string name;
    std::string audioFilePath;
    // Recording scratch buffers (audio-thread write only, never read by
    // any other thread while recording). Plain floats — no atomics needed
    // and no false-sharing cache lines. 2KB/track × 16 tracks = 32KB
    // saved vs the previous std::atomic<float>[BLOCK_SIZE] version.
    float bufferL[BLOCK_SIZE];
    float bufferR[BLOCK_SIZE];
    int writeIndex{0};
    std::atomic<float> volume{1.0f};
    std::atomic<float> pan{0.0f};
    std::atomic<bool> armed{false};
    std::atomic<bool> muted{false};
    std::atomic<bool> soloed{false};
    std::atomic<AudioBuffer*> audio{nullptr};
    std::atomic<uint64_t> clipStart{0};
    std::atomic<float> peakL{0.0f};
    std::atomic<float> peakR{0.0f};
};
