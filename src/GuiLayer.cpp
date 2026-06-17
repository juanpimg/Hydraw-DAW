#include "GuiLayer.h"
#include "AudioEngine.h"
#include "Plugin/PluginChain.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <cstring>

GuiLayer::GuiLayer(AudioEngine* engine)
    : m_engine(engine)
    , m_mixer(engine)
    , m_timeline(engine)
    , m_currentPath(std::filesystem::current_path())
{
}

void GuiLayer::init() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 5.0f;
    s.FrameRounding     = 5.0f;
    s.GrabRounding      = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.FramePadding      = ImVec2(8, 5);
    s.ItemSpacing       = ImVec2(8, 5);
    s.ItemInnerSpacing  = ImVec2(5, 5);
    s.WindowPadding     = ImVec2(10, 8);
    s.ScrollbarSize     = 8.0f;
    s.GrabMinSize       = 12.0f;

    ImVec4* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_WindowBg]           = ImVec4(0.05f, 0.05f, 0.07f, 1.00f);
    c[ImGuiCol_ChildBg]            = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
    c[ImGuiCol_FrameBg]            = ImVec4(0.11f, 0.11f, 0.14f, 1.00f);
    c[ImGuiCol_FrameBgHovered]     = ImVec4(0.16f, 0.16f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgActive]      = ImVec4(0.13f, 0.13f, 0.17f, 1.00f);
    c[ImGuiCol_Header]             = ImVec4(0.11f, 0.11f, 0.14f, 1.00f);
    c[ImGuiCol_HeaderHovered]      = ImVec4(0.16f, 0.20f, 0.30f, 1.00f);
    c[ImGuiCol_HeaderActive]       = ImVec4(0.13f, 0.18f, 0.28f, 1.00f);
    c[ImGuiCol_Button]             = ImVec4(0.13f, 0.13f, 0.17f, 1.00f);
    c[ImGuiCol_ButtonHovered]      = ImVec4(0.14f, 0.28f, 0.48f, 1.00f);
    c[ImGuiCol_ButtonActive]       = ImVec4(0.18f, 0.32f, 0.52f, 1.00f);
    c[ImGuiCol_Separator]          = ImVec4(0.16f, 0.16f, 0.20f, 1.00f);
    c[ImGuiCol_Text]               = ImVec4(0.90f, 0.90f, 0.93f, 1.00f);
    c[ImGuiCol_TextDisabled]       = ImVec4(0.45f, 0.45f, 0.50f, 1.00f);
    c[ImGuiCol_ScrollbarBg]        = ImVec4(0.02f, 0.02f, 0.03f, 0.40f);
    c[ImGuiCol_ScrollbarGrab]      = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.30f, 0.35f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.25f, 0.25f, 0.30f, 1.00f);
    c[ImGuiCol_SliderGrab]         = ImVec4(0.22f, 0.45f, 0.75f, 1.0f);
    c[ImGuiCol_SliderGrabActive]   = ImVec4(0.28f, 0.55f, 0.88f, 1.0f);
    c[ImGuiCol_CheckMark]          = ImVec4(0.30f, 0.60f, 0.95f, 1.0f);
    c[ImGuiCol_Border]             = ImVec4(0.18f, 0.18f, 0.22f, 0.60f);
    c[ImGuiCol_BorderShadow]       = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TitleBg]            = ImVec4(0.05f, 0.05f, 0.07f, 1.00f);
    c[ImGuiCol_TitleBgActive]      = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]   = ImVec4(0.04f, 0.04f, 0.06f, 1.00f);
    c[ImGuiCol_PopupBg]            = ImVec4(0.10f, 0.10f, 0.13f, 0.95f);

    refreshEntries();
}

void GuiLayer::refreshEntries() {
    m_entries.clear();
    for (auto& entry : std::filesystem::directory_iterator(m_currentPath)) {
        m_entries.push_back(entry);
    }
}

void GuiLayer::loadWavToSelectedTrack(const char* path) {
    if (m_engine->loadWavToTrack(m_selectedTrack, path)) {
        std::filesystem::path p(path);
        m_engine->getTrack(m_selectedTrack)->name = p.stem().string();
    }
}

void GuiLayer::render() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags f = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar
                       | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                       | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
                       | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("Main", nullptr, f);
    ImGui::PopStyleVar(2);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !ImGui::GetIO().WantTextInput) {
        if (m_engine->isPlaying()) m_engine->pause();
        else m_engine->play();
    }

    // Forward OS drops to timeline — timeline processes them during its own render
    bool hadDrops = false;
    if (!m_pendingDrops.empty()) {
        m_timeline.setPendingDrops(m_pendingDrops);
        m_pendingDrops.clear();
        hadDrops = true;
    }

    ImVec2 sz = ImGui::GetContentRegionAvail();
    float trH = 34.0f;
    ImGuiWindowFlags noScroll = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    // Transport
    ImGui::BeginChild("Trans", ImVec2(sz.x, trH), false, noScroll);
    renderTransportBar();
    ImGui::EndChild();

    float rem = sz.y - trH;
    float tlH = rem * 0.55f;
    float btH = rem - tlH;

    // Timeline
    ImGui::BeginChild("TL", ImVec2(sz.x, tlH), false, noScroll);
    m_timeline.render();
    m_selectedTrack = m_timeline.getSelectedTrack();
    ImGui::EndChild();

    // Divider
    ImDrawList* mDraw = ImGui::GetWindowDrawList();
    ImVec2 divPos = ImGui::GetCursorScreenPos();
    mDraw->AddLine(ImVec2(divPos.x, divPos.y + 2), ImVec2(divPos.x + sz.x, divPos.y + 2),
                   IM_COL32(25, 28, 35, 255), 1.5f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);

    // Auto-track: if drops weren't consumed, create a new track for them
    if (hadDrops && m_timeline.hasPendingDrops()) {
        auto paths = m_timeline.consumeDrops();
        int nt = m_engine->addTrack();
        if (nt >= 0) {
            for (auto& p : paths) {
                std::string ext = std::filesystem::path(p).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg") {
                    if (m_engine->loadWavToTrack(nt, p.c_str())) {
                        m_engine->getTrack(nt)->name = std::filesystem::path(p).stem().string();
                    }
                }
            }
            m_selectedTrack = nt;
        }
    }

    float bw = sz.x * 0.14f;
    float mw = sz.x * 0.28f;
    float fxw = sz.x - bw - mw;

    ImGui::BeginChild("FB", ImVec2(bw, btH), true, noScroll);
    renderFileBrowser();
    ImGui::EndChild();
    ImGui::SameLine(0, 0);

    ImGui::BeginChild("FX", ImVec2(fxw, btH), true, noScroll);
    renderFxChain();
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("MXR", ImVec2(mw, btH), true, noScroll);
    m_mixer.setSelectedTrack(m_selectedTrack);
    m_mixer.render();
    ImGui::EndChild();

    ImGui::PopStyleVar();
    ImGui::End();
}

void GuiLayer::renderTransportBar() {
    bool playing = m_engine->isPlaying();
    float winW = ImGui::GetWindowWidth();
    float cy = ImGui::GetCursorPosY();

    // --- Transport buttons (left) ---
    ImGui::SetCursorPos(ImVec2(4, cy));
    ImGui::PushStyleColor(ImGuiCol_Button, playing
        ? ImVec4(0.28f, 0.55f, 0.88f, 1.0f) : ImVec4(0.11f, 0.13f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, playing
        ? ImVec4(0.35f, 0.62f, 0.95f, 1.0f) : ImVec4(0.15f, 0.30f, 0.50f, 1.0f));
    if (ImGui::Button(playing ? " || " : " > ", ImVec2(42, 0))) {
        if (playing) m_engine->pause(); else m_engine->play();
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0, 4);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.13f, 0.17f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.15f, 0.15f, 1.0f));
    if (ImGui::Button(" # ", ImVec2(38, 0))) m_engine->stopTransport();
    ImGui::PopStyleColor(2);

    // --- Track buttons (right-1) ---
    ImGui::SetCursorPos(ImVec2(winW - 175, cy + 2));
    if (ImGui::SmallButton("+T")) { int n = m_engine->addTrack(); if (n >= 0) m_selectedTrack = n; }
    ImGui::SameLine(0, 2);
    if (ImGui::SmallButton("-T")) { m_engine->removeTrack(m_selectedTrack); if (m_selectedTrack >= m_engine->getTrackCount()) m_selectedTrack = m_engine->getTrackCount() - 1; }

    // --- Time digits (right-2, fixed X, nothing moves) ---
    uint64_t ph = m_engine->getPlayhead();
    double secs = (double)ph / SAMPLE_RATE;
    int m = (int)(secs / 60), s = (int)secs % 60, ms = (int)((secs - (int)secs) * 1000);
    ImGui::SetCursorPos(ImVec2(winW - 108, cy + 6));
    ImGui::TextDisabled("%02d:%02d.%03d", m, s, ms);

    // --- Status label (right-3, fixed X after time, absolutely independent) ---
    ImGui::SetCursorPos(ImVec2(winW - 38, cy + 6));
    if (playing)
        ImGui::TextColored(ImVec4(0.30f, 0.60f, 1.0f, 1.0f), "PLAY");
    else
        ImGui::TextColored(ImVec4(0.40f, 0.40f, 0.45f, 1.0f), "STOP");
}

void GuiLayer::renderFileBrowser() {
    ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.85f, 1.0f), "Browser");
    ImGui::SameLine(ImGui::GetWindowWidth() - 30);
    if (ImGui::SmallButton("..")) {
        m_currentPath = m_currentPath.parent_path();
        refreshEntries();
    }
    ImGui::TextDisabled("%.30s", m_currentPath.string().c_str());
    ImGui::Separator();

    ImGui::BeginChild("FList", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (auto& entry : m_entries) {
        auto path = entry.path();
        auto name = path.filename().string();
        if (entry.is_directory()) {
            if (ImGui::Selectable(name.c_str())) {
                m_currentPath = path;
                refreshEntries();
                break;
            }
        } else {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg") {
                bool sel = ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    std::string fp = path.string();
                    ImGui::SetDragDropPayload("FILE_PATH", fp.c_str(), fp.size() + 1);
                    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (sel && ImGui::IsMouseDoubleClicked(0))
                    loadWavToSelectedTrack(path.string().c_str());
            }
        }
    }
    ImGui::EndChild();
}

void GuiLayer::renderFxChain() {
    ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.85f, 1.0f), "Track %d", m_selectedTrack + 1);
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    Track* track = m_engine->getTrack(m_selectedTrack);
    ImGui::TextDisabled("%.12s", track->name.c_str());
    ImGui::Separator();

    AudioBuffer* buf = track->audio.load(std::memory_order_relaxed);
    if (buf && buf->frames > 0) {
        float dur = (float)buf->frames / SAMPLE_RATE;
        ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.55f, 1.0f), " %.1f sec ", dur);
    } else {
        ImGui::TextDisabled(" no audio ");
    }

    float vol = track->volume.load();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderFloat("Vol", &vol, 0.0f, 2.0f, "%.2f")) track->volume.store(vol);
    ImGui::SameLine();
    float pan = track->pan.load();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderFloat("Pan", &pan, -1.0f, 1.0f, pan < 0 ? "L%.1f" : pan > 0 ? "R%.1f" : "C"))
        track->pan.store(pan);

    bool mt = track->muted.load(), sl = track->soloed.load(), ar = track->armed.load();
    if (ImGui::Checkbox("M", &mt)) track->muted.store(mt);
    ImGui::SameLine();
    if (ImGui::Checkbox("S", &sl)) track->soloed.store(sl);
    ImGui::SameLine();
    if (ImGui::Checkbox("R", &ar)) track->armed.store(ar);

    ImGui::Separator();
    ImGui::TextDisabled("Plugins (CLAP)");
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Plugin")) {
        ImGui::OpenPopup("AddPluginPopup");
    }

    PluginChain* chain = m_engine->getPluginChain(m_selectedTrack);
    if (chain) {
        ImGui::BeginChild("PL", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        for (int i = 0; i < chain->getPluginCount(); ++i) {
            ImGui::PushID(i);
            bool bp = chain->isBypassed(i);
            if (ImGui::Checkbox("##bp", &bp)) chain->setBypass(i, bp);
            ImGui::SameLine(); ImGui::Text("%s", chain->getPluginName(i));
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
            if (ImGui::SmallButton("x")) chain->removePlugin(i--);
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    if (ImGui::BeginPopup("AddPluginPopup")) {
        ImGui::Text("Scan for CLAP plugins...");
        ImGui::Separator();
        static std::vector<std::string> cachedPlugins;
        static std::vector<std::string> cachedLibPaths;
        static bool scanned = false;
        if (!scanned) {
            cachedPlugins.clear();
            cachedLibPaths.clear();
            std::vector<std::string> searchPaths = {"./plugins", "/usr/lib/clap", "/usr/local/lib/clap"};
            for (auto& dirPath : searchPaths) {
                if (!std::filesystem::is_directory(dirPath)) continue;
                for (auto& entry : std::filesystem::directory_iterator(dirPath)) {
                    auto path = entry.path().string();
                    if (ClapHost::isClapLibrary(path.c_str())) {
                        void* lib = ClapHost::loadLibrary(path.c_str());
                        if (!lib) continue;
                        uint32_t count = ClapHost::getPluginCount(lib);
                        for (uint32_t i = 0; i < count; ++i) {
                            auto* desc = ClapHost::getPluginDescriptor(lib, i);
                            if (desc && desc->id) {
                                cachedPlugins.push_back(desc->id);
                                cachedLibPaths.push_back(path);
                            }
                        }
                        ClapHost::unloadLibrary(lib, path.c_str());
                    }
                }
            }
            scanned = true;
        }
        if (ImGui::SmallButton("Rescan")) { scanned = false; }
        ImGui::Separator();
        if (cachedPlugins.empty()) {
            ImGui::TextDisabled("No plugins found");
        } else {
            for (int i = 0; i < (int)cachedPlugins.size(); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(cachedPlugins[i].c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        PluginChain* pc = m_engine->getPluginChain(m_selectedTrack);
                        if (pc) {
                            pc->addPlugin(cachedLibPaths[i].c_str(), cachedPlugins[i].c_str());
                        }
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndPopup();
    }
}

void GuiLayer::dropFiles(int count, const char** paths) {
    for (int i = 0; i < count; ++i) m_pendingDrops.push_back(paths[i]);
}

void GuiLayer::shutdown() {}
