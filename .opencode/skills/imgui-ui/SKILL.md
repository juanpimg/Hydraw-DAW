---
name: imgui-ui
description: Best practices for Dear ImGui layout, sizing, scrollbars, Z-order, and drag-and-drop. Use this skill whenever modifying the UI of DawLite.
license: MIT
compatibility: opencode
---

## ImGui Layout Rules for DawLite

### 1. No scrollbars on outer panels
- Every outer `BeginChild` MUST have `ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse`
- Only allow scrollbars on inner content lists (file list, plugin list)
- Main window MUST have `NoScrollbar | NoScrollWithMouse`
- Use `PushStyleVar(ItemSpacing, 0)` inside the main window to prevent child spacing overflow

### 2. Fixed positioning over SameLine
- Use `ImGui::SetCursorPos(ImVec2(x_absoluta, y))` instead of `SameLine()` for right-aligned elements
- Calculate X from `GetWindowWidth()` to anchor elements to window edges
- Each independent UI section gets its own `SetCursorPos` — never chain positions
- Transport bar pattern: transport buttons = natural layout, everything else = absolute X

### 3. Widget Z-order for click handling
- In ImGui, the LAST drawn widget in the same window receives clicks FIRST
- Buttons (M/S/A, etc.) MUST be drawn AFTER any overlapping `InvisibleButton`
- Order: InvisibleButton (track-wide) → widgets (buttons on top)
- Use `SetCursorScreenPos` for absolute visual placement, `SetCursorPos` for relative within child

### 4. Drag-and-drop without ImGui DnD
- ImGui's `BeginDragDropSource/Target` conflicts with overlapping widgets
- Use manual hit-testing: `ImGui::IsMouseHoveringRect` + `ImGui::IsMouseClicked` before widgets consume clicks
- Track drag state manually with member variables
- Draw drag ghost at `GetMousePos()` during drag, commit on `IsMouseReleased`

### 5. Font loading
- `AddFontFromFileTTF()` MUST be called BEFORE `ImGui_ImplOpenGL3_Init()`
- This ensures the font atlas is built with the custom font included
- Provide fallback fonts for portability

### 6. Child window sizing
- `ImVec2(0, 0)` = fill remaining space
- `ImVec2(w, h)` = fixed size
- Always account for `ItemSpacing` when stacking children vertically
- Total height of stacked children must NOT exceed parent `ContentRegionAvail`

### 7. Z-order within BeginChild
- All children of the same BeginChild share Z-order
- Later `BeginChild` siblings are rendered on top
- Drag-drop detection must account for layered windows

### 8. Custom faders/meters
- Use `ImDrawList` for custom rendering (meters, fader tracks)
- Use `InvisibleButton` over the draw area for mouse interaction
- On `IsItemActive()`, read `GetMousePos()` to compute drag value
- Store float value locally, only commit to atomic on change
