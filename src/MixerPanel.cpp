#include "MixerPanel.h"
#include "AudioEngine.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>

MixerPanel::MixerPanel(AudioEngine* engine)
    : m_engine(engine)
{
}

static void drawFader(const char* label, ImVec2 pos, ImVec2 size, float* value) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.12f, 0.12f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.26f, 0.50f, 0.80f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.32f, 0.60f, 0.92f, 1.0f));
    ImGui::VSliderFloat(label, size, value, 0.0f, 1.5f, "");
    ImGui::PopStyleColor(4);

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        *value = 1.0f;
    }
}

static void drawMeter(ImDrawList* dl, ImVec2 pos, ImVec2 size, float level, bool master) {
    const int NUM_LEDS = 16;
    float h = size.y, w = size.x;
    float ledH = (h - (NUM_LEDS - 1)) / (float)NUM_LEDS;
    if (ledH < 1.0f) ledH = 1.0f;

    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
                      master ? IM_COL32(28, 12, 12, 255) : IM_COL32(16, 16, 18, 255));
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(50, 50, 55, 255));

    float norm = std::min(level / 1.5f, 1.0f);
    int ledsOn = (int)(norm * NUM_LEDS);
    if (ledsOn > NUM_LEDS) ledsOn = NUM_LEDS;

    for (int i = 0; i < NUM_LEDS; ++i) {
        float y0 = pos.y + h - (i + 1) * (ledH + 1.0f);
        float y1 = y0 + ledH;
        bool lit = (i < ledsOn);

        ImU32 col;
        float pct = (float)(i + 1) / NUM_LEDS;
        if (pct <= 0.70f)
            col = lit ? IM_COL32(40, 220, 60, 255) : IM_COL32(18, 45, 22, 200);
        else if (pct <= 0.90f)
            col = lit ? IM_COL32(240, 210, 40, 255) : IM_COL32(45, 40, 12, 200);
        else
            col = lit ? IM_COL32(240, 40, 40, 255) : IM_COL32(45, 12, 12, 200);

        dl->AddRectFilled(ImVec2(pos.x + 1, y0), ImVec2(pos.x + w - 1, y1), col);
    }
}

void MixerPanel::render() {
    ImVec2 sz = ImGui::GetContentRegionAvail();
    int trackCount = m_engine->getTrackCount();
    float barH = 26.0f;
    float contentH = sz.y - barH;
    int totalCh = trackCount + 1;
    float chW = 72.0f;
    float totalW = chW * totalCh;

    ImGuiWindowFlags noSc = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGuiWindowFlags mxF = noSc;
    if (totalW > sz.x) mxF = ImGuiWindowFlags_HorizontalScrollbar;

    ImGui::BeginChild("MXR_in", ImVec2(0, contentH), false, mxF);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 inner = ImGui::GetContentRegionAvail();
    if (totalW < inner.x) chW = inner.x / totalCh;

    for (int t = 0; t < trackCount; ++t) {
        Track* track = m_engine->getTrack(t);
        ImGui::PushID(t);

        char cn[16]; snprintf(cn, sizeof(cn), "Ch%d", t);
        ImGui::BeginChild(cn, ImVec2(chW, 0), true, noSc);
        ImVec2 av = ImGui::GetContentRegionAvail();

        // --- Track name (centered) ---
        ImGui::SetCursorPosX((chW - ImGui::CalcTextSize(track->name.c_str()).x) * 0.5f);
        ImGui::TextDisabled("%.10s", track->name.c_str());

        // --- Meter + Fader side by side ---
        float meterW = std::max(10.0f, av.x * 0.18f);
        float faderW = av.x - meterW - 4.0f;
        float stripH = std::max(av.y - 52.0f, 40.0f);
        ImVec2 stripPos = ImGui::GetCursorScreenPos();

        // Meter (left)
        float peak = std::max(track->peakL.load(std::memory_order_relaxed),
                               track->peakR.load(std::memory_order_relaxed));
        drawMeter(dl, stripPos, ImVec2(meterW, stripH), peak, false);

        // Fader (right of meter, SameLine)
        ImGui::SetCursorScreenPos(ImVec2(stripPos.x + meterW + 2, stripPos.y));
        float vol = track->volume.load(std::memory_order_relaxed), v0 = vol;
        drawFader("##vf", ImVec2(stripPos.x + meterW + 2, stripPos.y),
                  ImVec2(faderW, stripH), &vol);
        if (vol != v0) track->volume.store(vol);

        // --- dB readout (centered below strip) ---
        ImGui::SetCursorPosY(stripPos.y - ImGui::GetCursorScreenPos().y + stripH + 2);
        char dBuf[24];
        float dB = (vol > 0.0001f) ? 20.0f * std::log10(vol) : -INFINITY;
        if (std::isinf(dB))
            snprintf(dBuf, sizeof(dBuf), "-inf");
        else
            snprintf(dBuf, sizeof(dBuf), "%+.1f dB", dB);
        float tw = ImGui::CalcTextSize(dBuf).x;
        ImGui::SetCursorPosX((chW - tw) * 0.5f);
        ImGui::TextDisabled("%s", dBuf);

        // --- M/S/A buttons (centered at bottom) ---
        ImGui::SetCursorPosY(av.y - 18.0f);
        float btnSz = std::max((av.x - 10.0f) / 3.0f, 18.0f);
        float btnX = (av.x - btnSz * 3.0f - 4.0f) * 0.5f + 2.0f;
        ImGui::SetCursorPosX(btnX);
        bool mu = track->muted.load();
        if (mu) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.12f, 0.12f, 1.0f));
        if (ImGui::Button("M", ImVec2(btnSz, 0))) track->muted.store(!mu);
        if (mu) ImGui::PopStyleColor();
        ImGui::SameLine(0, 2);
        bool so = track->soloed.load();
        if (so) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.7f, 0.1f, 1.0f));
        if (ImGui::SmallButton("S")) track->soloed.store(!so);
        if (so) ImGui::PopStyleColor();
        ImGui::SameLine(0, 2);
        bool ar = track->armed.load();
        if (ar) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.12f, 0.12f, 1.0f));
        if (ImGui::SmallButton("A")) track->armed.store(!ar);
        if (ar) ImGui::PopStyleColor();

        ImGui::EndChild();
        ImGui::PopID();
        ImGui::SameLine(0, 0);
    }

    // --- Master channel ---
    ImGui::PushID(-1);
    ImGui::BeginChild("Mast", ImVec2(chW, 0), true, noSc);
    MasterBus* master = m_engine->getMaster();
    ImVec2 ma = ImGui::GetContentRegionAvail();

    ImGui::SetCursorPosX((chW - ImGui::CalcTextSize("Master").x) * 0.5f);
    ImGui::TextDisabled("Master");

    float meterW = std::max(10.0f, ma.x * 0.18f);
    float faderW = ma.x - meterW - 4.0f;
    float stripH = std::max(ma.y - 52.0f, 40.0f);
    ImVec2 mstripPos = ImGui::GetCursorScreenPos();

    float mpeak = std::max(master->peakL.load(std::memory_order_relaxed),
                            master->peakR.load(std::memory_order_relaxed));
    drawMeter(dl, mstripPos, ImVec2(meterW, stripH), mpeak, true);

    ImGui::SetCursorScreenPos(ImVec2(mstripPos.x + meterW + 2, mstripPos.y));
    float mVol = master->volume.load(std::memory_order_relaxed), mv0 = mVol;
    drawFader("##mvf", ImVec2(mstripPos.x + meterW + 2, mstripPos.y),
              ImVec2(faderW, stripH), &mVol);
    if (mVol != mv0) master->volume.store(mVol);

    ImGui::SetCursorPosY(mstripPos.y - ImGui::GetCursorScreenPos().y + stripH + 2);
    char mdBuf[24];
    float mDB = (mVol > 0.0001f) ? 20.0f * std::log10(mVol) : -INFINITY;
    if (std::isinf(mDB))
        snprintf(mdBuf, sizeof(mdBuf), "-inf");
    else
        snprintf(mdBuf, sizeof(mdBuf), "%+.1f dB", mDB);
    float mtw = ImGui::CalcTextSize(mdBuf).x;
    ImGui::SetCursorPosX((chW - mtw) * 0.5f);
    ImGui::TextDisabled("%s", mdBuf);

    ImGui::EndChild();
    ImGui::PopID();
    ImGui::EndChild();

    // --- Toolbar ---
    ImGui::BeginChild("MXR_bt", ImVec2(0, barH), false, noSc);
    ImGui::TextDisabled(" %d tracks", m_engine->getTrackCount());
    ImGui::EndChild();
}
