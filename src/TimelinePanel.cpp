#include "TimelinePanel.h"
#include "AudioEngine.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

TimelinePanel::TimelinePanel(AudioEngine* engine)
    : m_engine(engine)
{
}

static ImU32 makeClipColor(const std::string& name) {
    uint32_t h = 5381;
    for (char c : name) h = ((h << 5) + h) + (unsigned char)c;
    return IM_COL32(60 + (h & 0x3F), 120 + ((h >> 6) & 0x3F), 160 + ((h >> 12) & 0x3F), 220);
}

void TimelinePanel::render() {
    ImVec2 size = ImGui::GetContentRegionAvail();
    int trackCount = m_engine->getTrackCount();

    float maxSeconds = 5.0f;
    for (int t = 0; t < trackCount; ++t) {
        AudioBuffer* buf = m_engine->getTrack(t)->audio.load(std::memory_order_relaxed);
        if (buf && buf->frames > 0) {
            uint64_t cs = m_engine->getTrack(t)->clipStart.load(std::memory_order_relaxed);
            float secs = (float)(cs + buf->frames) / SAMPLE_RATE;
            if (secs > maxSeconds) maxSeconds = secs;
        }
    }
    float totalWidth = std::max(size.x, HEADER_W + (maxSeconds + 2.0f) * PX_PER_SEC);

    ImGui::BeginChild("TimelineScroll", ImVec2(0, 0), false,
                       ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float contentW = std::max(size.x, totalWidth);

    // --- Ruler ---
    dl->AddRectFilled(ImVec2(origin.x, origin.y),
                      ImVec2(origin.x + contentW, origin.y + RULER_H),
                      IM_COL32(40, 40, 45, 255));
    dl->AddLine(ImVec2(origin.x, origin.y + RULER_H),
                ImVec2(origin.x + contentW, origin.y + RULER_H),
                IM_COL32(80, 80, 85, 255));

    for (float sec = 0.0f; sec <= maxSeconds + 2.0f; sec += 1.0f) {
        float px = origin.x + HEADER_W + sec * PX_PER_SEC;
        dl->AddLine(ImVec2(px, origin.y + RULER_H - 10),
                    ImVec2(px, origin.y + RULER_H),
                    IM_COL32(150, 150, 155, 255));
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0fs", sec);
        dl->AddText(ImVec2(px + 3, origin.y + 3), IM_COL32(190, 190, 190, 255), buf);
    }

    {
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y));
        ImGui::InvisibleButton("##ruler", ImVec2(contentW, RULER_H));
        if (ImGui::IsItemClicked()) {
            float clickX = ImGui::GetMousePos().x - origin.x;
            if (clickX > HEADER_W) {
                float clickSecs = (clickX - HEADER_W) / PX_PER_SEC;
                if (clickSecs > 0.0f)
                    m_engine->setPlayhead((uint64_t)(clickSecs * SAMPLE_RATE));
            }
        }
    }

    ImGui::SetCursorPos(ImVec2(0, RULER_H));
    m_hoveredTrack = -1;

    // Compute track row screen positions
    float trackStartY[MAX_TRACKS];
    for (int t = 0; t < trackCount; ++t) {
        trackStartY[t] = origin.y + RULER_H + t * TRACK_H;
    }

    // Handle custom drag release
    bool dropNow = false;
    int dropTrack = -1;
    float dropMouseX = 0;

    if (m_dragTrack >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        dropNow = true;
        dropMouseX = ImGui::GetMousePos().x;
        for (int t = 0; t < trackCount; ++t) {
            if (ImGui::GetMousePos().y >= trackStartY[t] &&
                ImGui::GetMousePos().y < trackStartY[t] + TRACK_H) {
                dropTrack = t;
                break;
            }
        }
    }

    // Handle pending click release (no drag started)
    if (m_pendingClick && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (m_pendingTrack >= 0 && m_pendingTrack < trackCount) {
            m_engine->setPlayhead(m_pendingClickFrame);
            m_selectedTrack = m_pendingTrack;
        }
        m_pendingClick = false;
        m_pendingTrack = -1;
    }

    for (int t = 0; t < trackCount; ++t) {
        Track* track = m_engine->getTrack(t);
        AudioBuffer* buf = track->audio.load(std::memory_order_relaxed);
        uint64_t cs = track->clipStart.load(std::memory_order_relaxed);

        ImGui::PushID(t);
        float rowY = trackStartY[t] - origin.y; // window-local Y
        ImVec2 rowOrigin(origin.x, trackStartY[t]);
        bool selected = (t == m_selectedTrack);
        bool isBeingDragged = (m_dragTrack == t);

        // Compute clip rect if audio exists (before we render anything)
        // This is used to exclude clip area from track-wide click
        float clipAreaX0 = 0, clipAreaX1 = 0;
        bool hasAudio = (buf && buf->frames > 0);
        if (hasAudio) {
            clipAreaX0 = HEADER_W + (float)cs / SAMPLE_RATE * PX_PER_SEC;
            clipAreaX1 = clipAreaX0 + (float)buf->frames / SAMPLE_RATE * PX_PER_SEC;
        }

        if (ImGui::IsMouseHoveringRect(rowOrigin,
                ImVec2(rowOrigin.x + contentW, rowOrigin.y + TRACK_H))) {
            m_hoveredTrack = t;
        }

        // Row background
        ImU32 rowBg = selected ? IM_COL32(50, 50, 68, 255) : IM_COL32(32, 32, 36, 255);
        dl->AddRectFilled(ImVec2(rowOrigin.x, rowOrigin.y),
                          ImVec2(rowOrigin.x + contentW, rowOrigin.y + TRACK_H), rowBg);
        dl->AddLine(ImVec2(rowOrigin.x, rowOrigin.y),
                    ImVec2(rowOrigin.x + contentW, rowOrigin.y), IM_COL32(55, 55, 58, 255));
        dl->AddLine(ImVec2(rowOrigin.x, rowOrigin.y + TRACK_H - 1),
                    ImVec2(rowOrigin.x + contentW, rowOrigin.y + TRACK_H - 1),
                    IM_COL32(55, 55, 58, 255));

        // Track header
        ImU32 headerBg = selected ? IM_COL32(60, 60, 80, 255) : IM_COL32(42, 42, 50, 255);
        dl->AddRectFilled(ImVec2(rowOrigin.x, rowOrigin.y),
                          ImVec2(rowOrigin.x + HEADER_W, rowOrigin.y + TRACK_H), headerBg);
        dl->AddLine(ImVec2(rowOrigin.x + HEADER_W, rowOrigin.y),
                    ImVec2(rowOrigin.x + HEADER_W, rowOrigin.y + TRACK_H),
                    IM_COL32(70, 70, 75, 255));

        // Track name
        dl->AddText(ImVec2(rowOrigin.x + 26, rowOrigin.y + 2),
                    IM_COL32(210, 210, 210, 255), track->name.c_str());

        // --- Clip click/drag detection (BEFORE track-wide button) ---
        if (hasAudio && !isBeingDragged) {
            float cX = rowOrigin.x + HEADER_W + (float)cs / SAMPLE_RATE * PX_PER_SEC;
            float cW = (float)buf->frames / SAMPLE_RATE * PX_PER_SEC;
            ImVec2 rMin(cX, rowOrigin.y + 3);
            ImVec2 rMax(cX + cW, rowOrigin.y + TRACK_H - 3);
            bool hoverClip = ImGui::IsMouseHoveringRect(rMin, rMax);

            if (hoverClip && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                // Mouse pressed on clip — record pending, don't start drag yet
                m_pendingClick = true;
                m_pendingTrack = t;
                m_pendingOffX = ImGui::GetMousePos().x - cX;
                uint64_t offFrames = (uint64_t)(m_pendingOffX / PX_PER_SEC * SAMPLE_RATE);
                m_pendingClickFrame = cs + offFrames;
                m_dragOrigStart = cs;
                m_dragName = track->name;
            }

            if (m_pendingClick && m_pendingTrack == t && hoverClip &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
                // Mouse moved past threshold → promote to real drag
                m_dragTrack = t;
                m_dragOffX = m_pendingOffX;
                m_pendingClick = false;
                m_pendingTrack = -1;
            }
        }

        // Track-wide click target (select/seek) — excluding clip area
        ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x, rowOrigin.y));
        ImGui::InvisibleButton("##row", ImVec2(contentW, TRACK_H));
        if (ImGui::IsItemClicked()) {
            float clickX = ImGui::GetMousePos().x - rowOrigin.x;
            bool onClip = hasAudio && clickX >= clipAreaX0 && clickX <= clipAreaX1;
            if (!onClip) {
                if (clickX > HEADER_W) {
                    float clickSecs = (clickX - HEADER_W) / PX_PER_SEC;
                    if (clickSecs >= 0.0f)
                        m_engine->setPlayhead((uint64_t)(clickSecs * SAMPLE_RATE));
                    m_selectedTrack = t;
                } else {
                    m_selectedTrack = t;
                }
            }
        }
        // Accept file drops from browser onto this track
        if (ImGui::BeginDragDropTarget()) {
            const ImGuiPayload* fp = ImGui::AcceptDragDropPayload("FILE_PATH");
            if (fp) {
                const char* path = (const char*)fp->Data;
                if (m_engine->loadWavToTrack(t, path)) {
                    std::filesystem::path p(path);
                    m_engine->getTrack(t)->name = p.stem().string();
                }
            }
            ImGui::EndDragDropTarget();
        }

        // Process OS drops (files dragged from outside the program)
        if (!m_dropPaths.empty() &&
            ImGui::IsMouseHoveringRect(rowOrigin, ImVec2(rowOrigin.x + contentW, rowOrigin.y + TRACK_H))) {
            for (auto& dp : m_dropPaths) {
                std::string ext = std::filesystem::path(dp).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg") {
                    if (m_engine->loadWavToTrack(t, dp.c_str())) {
                        std::filesystem::path p(dp);
                        m_engine->getTrack(t)->name = p.stem().string();
                    }
                }
            }
            m_dropPaths.clear();
        }

        // Render clip on this track (unless it's being dragged)
        if (hasAudio && !isBeingDragged) {
            float clipX = rowOrigin.x + HEADER_W + (float)cs / SAMPLE_RATE * PX_PER_SEC;
            float clipW = (float)buf->frames / SAMPLE_RATE * PX_PER_SEC;
            float clipRight = std::min(clipX + clipW, rowOrigin.x + contentW);
            float visibleClipW = std::max(clipRight - clipX, 4.0f);
            ImU32 clipCol = makeClipColor(track->name);

            // Clip background
            dl->AddRectFilled(ImVec2(clipX, rowOrigin.y + 3),
                              ImVec2(clipRight, rowOrigin.y + TRACK_H - 3), clipCol);
            dl->AddRect(ImVec2(clipX, rowOrigin.y + 3),
                        ImVec2(clipRight, rowOrigin.y + TRACK_H - 3),
                        IM_COL32(255, 255, 255, 120), 1.5f);

            // Mini waveform (peak-cache accelerated)
            float wfMidY = rowOrigin.y + TRACK_H * 0.5f;
            float ampScale = (TRACK_H - 10.0f) * 0.4f;
            int numBars = (int)std::min(visibleClipW, 300.0f);
            if (numBars > 1 && !buf->peakCache.empty()) {
                for (int b = 0; b < numBars; ++b) {
                    int idx = (int)((float)b / numBars * buf->peakCache.size());
                    if (idx >= (int)buf->peakCache.size()) idx = (int)buf->peakCache.size() - 1;
                    float mx = buf->peakCache[idx];
                    float barH = std::min(mx * ampScale, ampScale);
                    float bx = clipX + (float)b * visibleClipW / numBars;
                    dl->AddLine(ImVec2(bx, wfMidY - barH), ImVec2(bx, wfMidY + barH),
                                IM_COL32(255, 255, 255, 160));
                }
            }
            dl->AddText(ImVec2(clipX + 4, rowOrigin.y + 5),
                        IM_COL32(255, 255, 255, 230), track->name.c_str());
        } else if (!hasAudio) {
            float placeX = rowOrigin.x + HEADER_W;
            dl->AddRectFilled(ImVec2(placeX + 4, rowOrigin.y + 10),
                              ImVec2(placeX + 180, rowOrigin.y + TRACK_H - 10),
                              IM_COL32(50, 50, 55, 180));
            dl->AddText(ImVec2(placeX + 8, rowOrigin.y + TRACK_H * 0.5f - 8),
                        IM_COL32(120, 120, 130, 255), "Drop WAV/MP3 here");
        }

        // M/S/A buttons — vertical stack in header
        float btnY = rowOrigin.y + 2;
        float btnH = 12.0f;
        float gap = 1.0f;
        ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + 4, btnY));
        bool mute = track->muted.load();
        if (mute) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
        if (ImGui::Button("M", ImVec2(18, btnH))) track->muted.store(!mute);
        if (mute) ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + 4, btnY + btnH + gap));
        bool solo = track->soloed.load();
        if (solo) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.6f, 0.0f, 1.0f));
        if (ImGui::Button("S", ImVec2(18, btnH))) track->soloed.store(!solo);
        if (solo) ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + 4, btnY + (btnH + gap) * 2));
        bool arm = track->armed.load();
        if (arm) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
        if (ImGui::Button("A", ImVec2(18, btnH))) track->armed.store(!arm);
        if (arm) ImGui::PopStyleColor();

        ImGui::SetCursorPos(ImVec2(0, rowY + TRACK_H));
        ImGui::PopID();
    }

    // Draw drag ghost
    if (m_dragTrack >= 0) {
        AudioBuffer* buf = m_engine->getTrack(m_dragTrack)->audio.load(std::memory_order_relaxed);
        if (buf && buf->frames > 0) {
            float clipW = (float)buf->frames / SAMPLE_RATE * PX_PER_SEC;
            float clipH = TRACK_H - 6;
            ImVec2 mouse = ImGui::GetMousePos();
            float dx = mouse.x - m_dragOffX;
            float dy = mouse.y - clipH * 0.5f;
            ImU32 c = makeClipColor(m_dragName);

            dl->AddRectFilled(ImVec2(dx, dy), ImVec2(dx + clipW, dy + clipH),
                              IM_COL32((c >> 0) & 0xFF, (c >> 8) & 0xFF,
                                       (c >> 16) & 0xFF, 180));
            dl->AddRect(ImVec2(dx, dy), ImVec2(dx + clipW, dy + clipH),
                        IM_COL32(255, 255, 255, 180), 1.5f);
            dl->AddText(ImVec2(dx + 4, dy + 2), IM_COL32(255, 255, 255, 220),
                        m_dragName.c_str());

            // Mini waveform in ghost (peak-cache accelerated)
            float midY = dy + clipH * 0.5f;
            float amp = clipH * 0.4f;
            int nb = (int)std::min(clipW, 300.0f);
            if (nb > 1 && !buf->peakCache.empty()) {
                for (int b = 0; b < nb; ++b) {
                    int idx = (int)((float)b / nb * buf->peakCache.size());
                    if (idx >= (int)buf->peakCache.size()) idx = (int)buf->peakCache.size() - 1;
                    float mx = buf->peakCache[idx];
                    float bh = std::min(mx * amp, amp);
                    dl->AddLine(ImVec2(dx + (float)b * clipW / nb, midY - bh),
                                ImVec2(dx + (float)b * clipW / nb, midY + bh),
                                IM_COL32(255, 255, 255, 140));
                }
            }
        }
    }

    // Commit drop
    if (dropNow && m_dragTrack >= 0) {
        if (dropTrack >= 0 && dropTrack != m_dragTrack) {
            m_engine->swapTrackAudio(m_dragTrack, dropTrack);
        }
        int finalTrack = (dropTrack >= 0) ? dropTrack : m_dragTrack;
        float relX = (dropMouseX - m_dragOffX) - origin.x - HEADER_W;
        if (relX < 0) relX = 0;
        uint64_t newStart = (uint64_t)(relX / PX_PER_SEC * SAMPLE_RATE);
        m_engine->setClipStart(finalTrack, newStart);
        m_dragTrack = -1;
    }

    // --- Playback cursor ---
    uint64_t playhead = m_engine->getPlayhead();
    if (m_engine->isPlaying() || playhead > 0) {
        float cursorX = origin.x + HEADER_W + (float)playhead / SAMPLE_RATE * PX_PER_SEC;
        dl->AddLine(ImVec2(cursorX, origin.y + RULER_H),
                    ImVec2(cursorX, origin.y + RULER_H + trackCount * TRACK_H),
                    IM_COL32(255, 210, 80, 240), 2.0f);
        dl->AddTriangleFilled(ImVec2(cursorX, origin.y + RULER_H),
                              ImVec2(cursorX - 5, origin.y + RULER_H - 8),
                              ImVec2(cursorX + 5, origin.y + RULER_H - 8),
                              IM_COL32(255, 210, 80, 240));
    }

    ImGui::EndChild();
}
