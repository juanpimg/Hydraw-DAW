#pragma once
#include "Core/Types.h"

class AudioEngine;

class TimelinePanel {
public:
    TimelinePanel(AudioEngine* engine);
    void render();
    int getSelectedTrack() const { return m_selectedTrack; }
    int getTrackAtMouse() const { return m_hoveredTrack; }
    void setPendingDrops(const std::vector<std::string>& paths) { m_dropPaths = paths; }
    bool hasPendingDrops() const { return !m_dropPaths.empty(); }
    std::vector<std::string> consumeDrops() { auto v = std::move(m_dropPaths); m_dropPaths.clear(); return v; }

private:
    AudioEngine* m_engine;
    int m_selectedTrack{0};
    int m_hoveredTrack{-1};

    // Custom drag state
    int  m_dragTrack{-1};
    float m_dragOffX{0};
    uint64_t m_dragOrigStart{0};
    std::string m_dragName;

    // Pending click (distinguishes single click from drag)
    int  m_pendingTrack{-1};
    float m_pendingOffX{0};
    uint64_t m_pendingClickFrame{0};
    bool m_pendingClick{false};

    // OS drop paths (processed during render)
    std::vector<std::string> m_dropPaths;

    static constexpr float PX_PER_SEC = 100.0f;
    static constexpr float TRACK_H = 44.0f;
    static constexpr float RULER_H = 22.0f;
    static constexpr float HEADER_W = 130.0f;
};
