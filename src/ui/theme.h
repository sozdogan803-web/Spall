// Windows Classic (95) and Windows XP Luna (Blue) looks for the Dear ImGui interface: the ImGui
// style colours and metrics, the UI fonts, and the draw primitives every themed widget uses
// (bevels, gradients, rounded frames, captions), so adding a theme does not touch the panels.
#pragma once

#include "imgui.h"

namespace ui {

enum class Theme { Classic, Luna, Count };
const char* themeName(Theme t);  // "Windows Classic", "Windows XP Luna"
const char* themeId(Theme t);    // "classic", "luna" (spall.ini)
bool parseTheme(const char* id, Theme& out);

struct Palette {
    // 3D object colours (Classic names; Luna maps its own scheme onto them).
    ImU32 face, highlight, light, shadow, dark;
    ImU32 field;          // text-field and list background
    ImU32 text, textDisabled, textSelected;
    ImU32 selection;      // list and menu selection
    ImU32 captionActive[3], captionInactive[3];  // title bar gradient: top, middle, bottom
    ImU32 captionText, captionTextInactive;
    ImU32 fieldBorder;    // Luna text-field border
    ImU32 buttonBorder, buttonTop, buttonBottom, hoverRim;  // Luna buttons
    ImU32 groupBorder, groupCaption;
    ImU32 progress;
    ImU32 plotBg, plotGrid, plotLine, plotLine2, plotMarker;
    // Text colours readable on the face colour.
    ImVec4 good, warn, bad, accent;
};

// Applies the theme to the current ImGui context. `scale` is the monitor content scale.
void setTheme(Theme t, float scale);
Theme theme();
const Palette& palette();
float px();     // one bevel pixel at the current scale (at least 1)
float scale();  // monitor content scale passed to setTheme

// Loads Microsoft Sans Serif or Tahoma for UI text, Tahoma Bold for captions and Lucida
// Console for numbers from %WINDIR%\Fonts; the ImGui default font stands in for any missing.
void loadFonts();
ImFont* uiFont();
ImFont* boldFont();
ImFont* monoFont();
float fontSize();  // unscaled UI font size, px

// ---- Draw primitives (all coordinates in screen pixels; [a, b) is the outer rectangle) ----

enum class Edge {
    Raised,       // button and window frame, 2 px
    Sunken,       // text field, list view and viewport frame, 2 px
    Pressed,      // pushed button, 2 px
    ShallowRaised,  // tool button hover, 1 px
    ShallowSunken,  // status pane and pressed tool button, 1 px
};
void bevel(ImDrawList* dl, ImVec2 a, ImVec2 b, Edge edge);
void fillRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 colour);
void verticalGradient(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom);
// Three-stop vertical gradient with the middle stop at fraction `mid` of the height.
void verticalGradient3(ImDrawList* dl, ImVec2 a, ImVec2 b, const ImU32 stops[3], float mid);
void roundedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 colour, float rounding, float thickness = 1.0f);
// Horizontal etched line (shadow over highlight).
void etchedLine(ImDrawList* dl, float x0, float x1, float y);
// Text-field frame and background: sunken bevel (Classic) or a thin blue border (Luna).
void fieldFrame(ImDrawList* dl, ImVec2 a, ImVec2 b);
// Button background and frame.
void buttonFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, bool hovered, bool pressed, bool isDefault = false);
// Title bar: gradient (or solid) background and the caption text in bold. Rounded top corners
// in Luna when `roundTop`.
void captionBar(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* text, bool active, bool roundTop);
// Group box frame with its title.
void groupFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* title);
// Text with the theme's selected-text colour on a selection background spanning [a, b).
void selectionFill(ImDrawList* dl, ImVec2 a, ImVec2 b);

ImU32 withAlpha(ImU32 colour, float alpha);
ImU32 lerpColour(ImU32 a, ImU32 b, float t);

} // namespace ui
