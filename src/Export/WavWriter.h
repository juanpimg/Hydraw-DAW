#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct WavWriter {
    static bool write16bit(const char* path, const float* samples, uint64_t numFrames, int channels, int sampleRate) {
        FILE* f = fopen(path, "wb");
        if (!f) return false;
        uint32_t dataBytes = (uint32_t)(numFrames * channels * 2);
        uint32_t chunkSize = 36 + dataBytes;
        uint32_t fmtSize = 16;
        uint16_t audioFmt = 1;
        uint16_t numCh = (uint16_t)channels;
        uint32_t sRate = (uint32_t)sampleRate;
        uint32_t byteRate = sRate * channels * 2;
        uint16_t blockAlign = (uint16_t)(channels * 2);
        uint16_t bitsPerSample = 16;

        fwrite("RIFF", 1, 4, f);
        fwrite(&chunkSize, 4, 1, f);
        fwrite("WAVE", 1, 4, f);
        fwrite("fmt ", 1, 4, f);
        fwrite(&fmtSize, 4, 1, f);
        fwrite(&audioFmt, 2, 1, f);
        fwrite(&numCh, 2, 1, f);
        fwrite(&sRate, 4, 1, f);
        fwrite(&byteRate, 4, 1, f);
        fwrite(&blockAlign, 2, 1, f);
        fwrite(&bitsPerSample, 2, 1, f);
        fwrite("data", 1, 4, f);
        fwrite(&dataBytes, 4, 1, f);

        for (uint64_t i = 0; i < numFrames * channels; ++i) {
            float s = samples[i];
            if (s > 1.0f) s = 1.0f;
            else if (s < -1.0f) s = -1.0f;
            int16_t val = (int16_t)(s * 32767.0f);
            fwrite(&val, 2, 1, f);
        }
        fclose(f);
        return true;
    }

    static bool writeFloat32(const char* path, const float* samples, uint64_t numFrames, int channels, int sampleRate) {
        FILE* f = fopen(path, "wb");
        if (!f) return false;
        uint32_t dataBytes = (uint32_t)(numFrames * channels * 4);
        uint32_t chunkSize = 36 + dataBytes;
        uint32_t fmtSize = 16;
        uint16_t audioFmt = 3;
        uint16_t numCh = (uint16_t)channels;
        uint32_t sRate = (uint32_t)sampleRate;
        uint32_t byteRate = sRate * channels * 4;
        uint16_t blockAlign = (uint16_t)(channels * 4);
        uint16_t bitsPerSample = 32;

        fwrite("RIFF", 1, 4, f);
        fwrite(&chunkSize, 4, 1, f);
        fwrite("WAVE", 1, 4, f);
        fwrite("fmt ", 1, 4, f);
        fwrite(&fmtSize, 4, 1, f);
        fwrite(&audioFmt, 2, 1, f);
        fwrite(&numCh, 2, 1, f);
        fwrite(&sRate, 4, 1, f);
        fwrite(&byteRate, 4, 1, f);
        fwrite(&blockAlign, 2, 1, f);
        fwrite(&bitsPerSample, 2, 1, f);
        fwrite("data", 1, 4, f);
        fwrite(&dataBytes, 4, 1, f);
        fwrite(samples, sizeof(float), numFrames * channels, f);
        fclose(f);
        return true;
    }
};
