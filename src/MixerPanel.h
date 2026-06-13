#pragma once
#include "Core/Types.h"

class AudioEngine;

class MixerPanel {
public:
    MixerPanel(AudioEngine* engine);
    void render();
    void setSelectedTrack(int t) { m_selectedTrack = t; }

private:
    AudioEngine* m_engine;
    int m_selectedTrack{0};
};
