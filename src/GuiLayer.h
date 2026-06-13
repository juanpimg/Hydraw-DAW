#pragma once
#include "Core/Types.h"
#include "MixerPanel.h"
#include "TimelinePanel.h"
#include <string>
#include <vector>
#include <filesystem>

class AudioEngine;

class GuiLayer {
public:
    GuiLayer(AudioEngine* engine);
    ~GuiLayer() = default;

    void init();
    void render();
    void shutdown();
    void dropFiles(int count, const char** paths);

private:
    void renderFileBrowser();
    void renderFxChain();
    void renderTransportBar();

    AudioEngine* m_engine;
    MixerPanel m_mixer;
    TimelinePanel m_timeline;
    int m_selectedTrack{0};

    std::filesystem::path m_currentPath;
    std::vector<std::filesystem::directory_entry> m_entries;
    char m_wavPathBuffer[512]{0};
    void refreshEntries();
    void loadWavToSelectedTrack(const char* path);

    std::vector<std::string> m_pendingDrops;
};
