#include "ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace ui {
namespace {

constexpr ImU32 rgb(uint32_t hex) { return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, 255); }
ImVec4 vec(uint32_t hex, float alpha = 1.0f) {
    return {((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha};
}

// Spec §8.3 colours.
const Palette kClassic = {
    rgb(0xC0C0C0), rgb(0xFFFFFF), rgb(0xDFDFDF), rgb(0x808080), rgb(0x000000),
    rgb(0xFFFFFF),
    rgb(0x000000), rgb(0x808080), rgb(0xFFFFFF),
    rgb(0x000080),
    {rgb(0x000080), rgb(0x000080), rgb(0x000080)}, {rgb(0x808080), rgb(0x808080), rgb(0x808080)},
    rgb(0xFFFFFF), rgb(0xC0C0C0),
    rgb(0x808080),
    rgb(0x000000), rgb(0xC0C0C0), rgb(0xC0C0C0), rgb(0x000000),
    rgb(0x808080), rgb(0x000000),
    rgb(0x000080),
    rgb(0xFFFFFF), rgb(0xE0E0E0), rgb(0x000080), rgb(0xC00000), rgb(0x008000),
    vec(0x007000), vec(0x8A5A00), vec(0xC00000), vec(0x000080),
};

const Palette kLuna = {
    rgb(0xECE9D8), rgb(0xFFFFFF), rgb(0xF1EFE2), rgb(0xACA899), rgb(0x716F64),
    rgb(0xFFFFFF),
    rgb(0x000000), rgb(0xACA899), rgb(0xFFFFFF),
    rgb(0x316AC5),
    {rgb(0x0058EE), rgb(0x3A93FF), rgb(0x0058EE)}, {rgb(0x7A96DF), rgb(0xA9C1F2), rgb(0x7A96DF)},
    rgb(0xFFFFFF), rgb(0xD8E4F8),
    rgb(0x7F9DB9),
    rgb(0x003C74), rgb(0xFFFFFF), rgb(0xECEBE6), rgb(0xF8B330),
    rgb(0xD0D0BF), rgb(0x0046D5),
    rgb(0x2FB42F),
    rgb(0xFFFFFF), rgb(0xE6E4D8), rgb(0x316AC5), rgb(0xE0601B), rgb(0x21A121),
    vec(0x1E7A1E), vec(0xB35A00), vec(0xC42B1C), vec(0x0046D5),
};

Theme gTheme = Theme::Classic;
float gScale = 1.0f, gPx = 1.0f;
ImFont* gClassicFont = nullptr;
ImFont* gLunaFont = nullptr;
ImFont* gBold = nullptr;
ImFont* gMono = nullptr;
constexpr float kFontSize = 13.0f;  // ImGui size (ascent - descent) of an 8 pt Windows UI font

ImFont* addFont(const char* file) {
    const char* windir = std::getenv("WINDIR");
    const std::filesystem::path path = std::filesystem::path(windir ? windir : "C:/Windows") / "Fonts" / file;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return nullptr;
    ImFontConfig config;
    config.PixelSnapH = true;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str(), kFontSize, &config);
}

void setColours(ImGuiStyle& st, Theme t) {
    ImVec4* c = st.Colors;
    const bool classic = t == Theme::Classic;
    const ImVec4 face = classic ? vec(0xC0C0C0) : vec(0xECE9D8);
    const ImVec4 sel = classic ? vec(0x000080) : vec(0x316AC5);
    const auto tint = [&](float alpha) { return ImVec4{sel.x, sel.y, sel.z, alpha}; };
    c[ImGuiCol_Text] = vec(0x000000);
    c[ImGuiCol_TextDisabled] = classic ? vec(0x808080) : vec(0xACA899);
    c[ImGuiCol_WindowBg] = face;
    c[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    c[ImGuiCol_PopupBg] = classic ? face : vec(0xFFFFFF);
    c[ImGuiCol_Border] = classic ? vec(0x808080) : vec(0x7F9DB9);
    c[ImGuiCol_BorderShadow] = classic ? vec(0xFFFFFF) : ImVec4{0, 0, 0, 0};
    c[ImGuiCol_FrameBg] = c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_FrameBgActive] = vec(0xFFFFFF);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgCollapsed] = classic ? vec(0x808080) : vec(0x7A96DF);
    c[ImGuiCol_TitleBgActive] = classic ? vec(0x000080) : vec(0x0058EE);
    c[ImGuiCol_MenuBarBg] = face;
    c[ImGuiCol_ScrollbarBg] = classic ? vec(0xDFDFDF) : vec(0xF4F3EE);
    c[ImGuiCol_ScrollbarGrab] = classic ? vec(0xA8A8A8) : vec(0xC1D3FB);
    c[ImGuiCol_ScrollbarGrabHovered] = classic ? vec(0x989898) : vec(0xD3E1FD);
    c[ImGuiCol_ScrollbarGrabActive] = classic ? vec(0x808080) : vec(0xA9C4F8);
    c[ImGuiCol_CheckMark] = classic ? vec(0x000000) : vec(0x21A121);
    c[ImGuiCol_CheckboxSelectedBg] = vec(0xFFFFFF);
    c[ImGuiCol_SliderGrab] = classic ? vec(0x808080) : vec(0x7F9DB9);
    c[ImGuiCol_SliderGrabActive] = classic ? vec(0x000080) : vec(0x316AC5);
    c[ImGuiCol_Button] = classic ? face : vec(0xF4F3EE);
    c[ImGuiCol_ButtonHovered] = classic ? vec(0xCACACA) : vec(0xFFF0CF);
    c[ImGuiCol_ButtonActive] = classic ? vec(0xB0B0B0) : vec(0xE3E2DA);
    c[ImGuiCol_Header] = tint(0.22f);
    c[ImGuiCol_HeaderHovered] = tint(0.32f);
    c[ImGuiCol_HeaderActive] = tint(0.45f);
    c[ImGuiCol_Separator] = classic ? vec(0x808080) : vec(0xACA899);
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = sel;
    c[ImGuiCol_ResizeGrip] = tint(0.15f);
    c[ImGuiCol_ResizeGripHovered] = tint(0.4f);
    c[ImGuiCol_ResizeGripActive] = sel;
    c[ImGuiCol_InputTextCursor] = vec(0x000000);
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = classic ? vec(0xB4B4B4) : vec(0xF4F3EE);
    c[ImGuiCol_TabHovered] = classic ? vec(0xCCCCCC) : vec(0xFFF0CF);
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = classic ? face : vec(0xFCFCFE);
    c[ImGuiCol_TabSelectedOverline] = c[ImGuiCol_TabDimmedSelectedOverline] = classic ? ImVec4{0, 0, 0, 0} : vec(0xE68B2C);
    c[ImGuiCol_PlotLines] = c[ImGuiCol_PlotHistogram] = sel;
    c[ImGuiCol_PlotLinesHovered] = c[ImGuiCol_PlotHistogramHovered] = classic ? vec(0xC00000) : vec(0xE0601B);
    c[ImGuiCol_TableHeaderBg] = classic ? face : vec(0xEBEADB);
    c[ImGuiCol_TableBorderStrong] = classic ? vec(0x808080) : vec(0xACA899);
    c[ImGuiCol_TableBorderLight] = classic ? vec(0xDFDFDF) : vec(0xE2E1D6);
    c[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
    c[ImGuiCol_TableRowBgAlt] = {0, 0, 0, 0.035f};
    c[ImGuiCol_TextLink] = classic ? vec(0x0000FF) : vec(0x0046D5);
    c[ImGuiCol_TextSelectedBg] = tint(0.35f);
    c[ImGuiCol_DragDropTarget] = sel;
    c[ImGuiCol_NavCursor] = sel;
    c[ImGuiCol_ModalWindowDimBg] = {0, 0, 0, 0.15f};
}

void frameEdges(ImDrawList* dl, ImVec2 a, ImVec2 b, float inset, ImU32 topLeft, ImU32 bottomRight) {
    const float p = gPx, k = inset;
    dl->AddRectFilled({a.x + k, a.y + k}, {b.x - k - p, a.y + k + p}, topLeft);
    dl->AddRectFilled({a.x + k, a.y + k}, {a.x + k + p, b.y - k - p}, topLeft);
    dl->AddRectFilled({a.x + k, b.y - k - p}, {b.x - k, b.y - k}, bottomRight);
    dl->AddRectFilled({b.x - k - p, a.y + k}, {b.x - k, b.y - k}, bottomRight);
}

ImVec2 snap(ImVec2 v) { return {std::floor(v.x), std::floor(v.y)}; }

} // namespace

const char* themeName(Theme t) { return t == Theme::Luna ? "Windows XP Luna" : "Windows Classic"; }
const char* themeId(Theme t) { return t == Theme::Luna ? "luna" : "classic"; }
bool parseTheme(const char* id, Theme& out) {
    for (int i = 0; i < static_cast<int>(Theme::Count); ++i)
        if (std::strcmp(id, themeId(static_cast<Theme>(i))) == 0) {
            out = static_cast<Theme>(i);
            return true;
        }
    return false;
}

Theme theme() { return gTheme; }
const Palette& palette() { return gTheme == Theme::Luna ? kLuna : kClassic; }
float px() { return gPx; }
float scale() { return gScale; }
float fontSize() { return kFontSize; }

void loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    gClassicFont = addFont("micross.ttf");
    gLunaFont = addFont("tahoma.ttf");
    if (!gClassicFont) gClassicFont = gLunaFont;
    if (!gLunaFont) gLunaFont = gClassicFont;
    if (!gClassicFont) {
        ImFontConfig config;
        config.SizePixels = kFontSize;
        gClassicFont = gLunaFont = io.Fonts->AddFontDefault(&config);
    }
    gBold = addFont("tahomabd.ttf");
    if (!gBold) gBold = gClassicFont;
    gMono = addFont("lucon.ttf");
    if (!gMono) gMono = addFont("consola.ttf");
    if (!gMono) gMono = gClassicFont;
    io.FontDefault = gClassicFont;
}

ImFont* uiFont() { return gTheme == Theme::Luna ? gLunaFont : gClassicFont; }
ImFont* boldFont() { return gBold; }
ImFont* monoFont() { return gMono; }

void setTheme(Theme t, float contentScale) {
    gTheme = t;
    gScale = std::clamp(contentScale, 0.5f, 4.0f);
    gPx = std::max(1.0f, std::floor(gScale + 0.25f));
    ImGuiStyle st;
    ImGui::StyleColorsLight(&st);
    setColours(st, t);
    const bool luna = t == Theme::Luna;
    st.WindowPadding = {6, 6};
    st.FramePadding = {4, 3};
    st.CellPadding = {4, 2};
    st.ItemSpacing = {6, 4};
    st.ItemInnerSpacing = {4, 4};
    st.IndentSpacing = 16;
    st.ScrollbarSize = 16;
    st.GrabMinSize = 10;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 0;
    st.PopupBorderSize = 1;
    st.FrameBorderSize = 1;
    st.TabBorderSize = 1;
    st.WindowRounding = st.ChildRounding = st.PopupRounding = st.FrameRounding = 0;
    st.GrabRounding = luna ? 2.0f : 0.0f;
    st.ScrollbarRounding = luna ? 3.0f : 0.0f;
    st.TabRounding = luna ? 3.0f : 0.0f;
    st.WindowTitleAlign = {0, 0.5f};
    st.SeparatorTextBorderSize = 1;
    st.AntiAliasedFill = false;  // crisp pixel edges
    st.AntiAliasedLines = true;  // plot curves
    st.ScaleAllSizes(gScale);
    st.FontScaleDpi = gScale;
    st.FontSizeBase = kFontSize;
    ImGui::GetStyle() = st;
    if (uiFont()) ImGui::GetIO().FontDefault = uiFont();
}

ImU32 withAlpha(ImU32 colour, float alpha) {
    const ImU32 a = static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * ((colour >> IM_COL32_A_SHIFT) & 0xFF) + 0.5f);
    return (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

ImU32 lerpColour(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    ImU32 out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
        out |= static_cast<ImU32>(x + (y - x) * t + 0.5f) << shift;
    }
    return out;
}

void fillRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 colour) { dl->AddRectFilled(snap(a), snap(b), colour); }

void bevel(ImDrawList* dl, ImVec2 a, ImVec2 b, Edge edge) {
    a = snap(a);
    b = snap(b);
    const Palette& p = palette();
    if (gTheme == Theme::Luna) {
        switch (edge) {
        case Edge::Raised:
        case Edge::ShallowRaised: frameEdges(dl, a, b, 0, p.highlight, p.shadow); break;
        case Edge::Sunken: dl->AddRect(a, b, p.fieldBorder, 0.0f, 0, gPx); break;
        case Edge::Pressed:
        case Edge::ShallowSunken: frameEdges(dl, a, b, 0, p.shadow, p.highlight); break;
        }
        return;
    }
    switch (edge) {
    case Edge::Raised:
        frameEdges(dl, a, b, 0, p.highlight, p.dark);
        frameEdges(dl, a, b, gPx, p.light, p.shadow);
        break;
    case Edge::Sunken:
        frameEdges(dl, a, b, 0, p.shadow, p.highlight);
        frameEdges(dl, a, b, gPx, p.dark, p.light);
        break;
    case Edge::Pressed:
        frameEdges(dl, a, b, 0, p.dark, p.highlight);
        frameEdges(dl, a, b, gPx, p.shadow, p.light);
        break;
    case Edge::ShallowRaised: frameEdges(dl, a, b, 0, p.highlight, p.shadow); break;
    case Edge::ShallowSunken: frameEdges(dl, a, b, 0, p.shadow, p.highlight); break;
    }
}

void verticalGradient(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom) {
    a = snap(a);
    b = snap(b);
    dl->AddRectFilledMultiColor(a, b, top, top, bottom, bottom);
}

void verticalGradient3(ImDrawList* dl, ImVec2 a, ImVec2 b, const ImU32 stops[3], float mid) {
    a = snap(a);
    b = snap(b);
    const float y = std::floor(a.y + (b.y - a.y) * mid);
    dl->AddRectFilledMultiColor(a, {b.x, y}, stops[0], stops[0], stops[1], stops[1]);
    dl->AddRectFilledMultiColor({a.x, y}, b, stops[1], stops[1], stops[2], stops[2]);
}

void roundedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 colour, float rounding, float thickness) {
    dl->AddRect(snap(a), snap(b), colour, rounding, 0, thickness);
}

void etchedLine(ImDrawList* dl, float x0, float x1, float y) {
    const Palette& p = palette();
    y = std::floor(y);
    dl->AddRectFilled({std::floor(x0), y}, {std::floor(x1), y + gPx}, p.shadow);
    dl->AddRectFilled({std::floor(x0), y + gPx}, {std::floor(x1), y + 2 * gPx}, p.highlight);
}

void fieldFrame(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const Palette& p = palette();
    fillRect(dl, a, b, p.field);
    bevel(dl, a, b, Edge::Sunken);
}

void buttonFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, bool hovered, bool pressed, bool isDefault) {
    a = snap(a);
    b = snap(b);
    const Palette& p = palette();
    if (gTheme == Theme::Classic) {
        dl->AddRectFilled(a, b, p.face);
        if (isDefault) {
            dl->AddRect(a, b, p.dark, 0.0f, 0, gPx);
            a = {a.x + gPx, a.y + gPx};
            b = {b.x - gPx, b.y - gPx};
        }
        bevel(dl, a, b, pressed ? Edge::Pressed : Edge::Raised);
        return;
    }
    const float r = 3.0f * gScale, k = 2.0f * gPx;
    dl->AddRectFilled(a, b, pressed ? rgb(0xE3E2DA) : p.buttonBottom, r);
    const ImU32 top = pressed ? rgb(0xE3E2DA) : p.buttonTop, bottom = pressed ? rgb(0xF7F7F4) : p.buttonBottom;
    dl->AddRectFilledMultiColor({a.x + k, a.y + k}, {b.x - k, b.y - k}, top, top, bottom, bottom);
    if (hovered && !pressed) dl->AddRect({a.x + gPx, a.y + gPx}, {b.x - gPx, b.y - gPx}, p.hoverRim, r, 0, 2.0f * gPx);
    else if (isDefault) dl->AddRect({a.x + gPx, a.y + gPx}, {b.x - gPx, b.y - gPx}, rgb(0x98B8F0), r, 0, 2.0f * gPx);
    dl->AddRect(a, b, p.buttonBorder, r, 0, gPx);
}

void captionBar(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* text, bool active, bool roundTop) {
    a = snap(a);
    b = snap(b);
    const Palette& p = palette();
    const ImU32* stops = active ? p.captionActive : p.captionInactive;
    if (gTheme == Theme::Classic) {
        dl->AddRectFilled(a, b, stops[0]);
    } else {
        // Row by row, so the rounded top corners and the gradient bands come out together.
        const float r = roundTop ? 7.0f * gScale : 0.0f, h = b.y - a.y;
        for (float y = a.y; y < b.y; y += 1.0f) {
            const float f = (y - a.y + 0.5f) / h;
            const ImU32 c = f < 0.22f ? lerpColour(stops[0], stops[1], f / 0.22f) : lerpColour(stops[1], stops[2], (f - 0.22f) / 0.5f);
            float inset = 0.0f;
            const float dy = r - (y - a.y) - 0.5f;
            if (dy > 0.0f) inset = std::floor(r - std::sqrt(std::max(r * r - dy * dy, 0.0f)) + 0.5f);
            dl->AddRectFilled({a.x + inset, y}, {b.x - inset, y + 1.0f}, c);
        }
    }
    if (!text || !*text) return;
    ImGui::PushFont(boldFont(), 0.0f);
    const float size = ImGui::GetFontSize();
    const ImVec2 pos{a.x + 4.0f * gScale, std::floor(a.y + 0.5f * (b.y - a.y - size))};
    const ImU32 colour = active ? p.captionText : p.captionTextInactive;
    if (gTheme == Theme::Luna && active) dl->AddText({pos.x + gPx, pos.y + gPx}, rgb(0x0A1E6E), text);
    dl->AddText(pos, colour, text);
    ImGui::PopFont();
}

void groupFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* title) {
    a = snap(a);
    b = snap(b);
    const Palette& p = palette();
    const float half = std::floor(0.5f * ImGui::GetFontSize());
    const float y = a.y + half;
    const ImVec2 textSize = title && *title ? ImGui::CalcTextSize(title) : ImVec2{0, 0};
    const float tx = a.x + 8.0f * gScale;
    if (gTheme == Theme::Classic) {
        dl->AddRect({a.x + gPx, y + gPx}, b, p.highlight, 0.0f, 0, gPx);
        dl->AddRect({a.x, y}, {b.x - gPx, b.y - gPx}, p.shadow, 0.0f, 0, gPx);
    } else {
        dl->AddRect({a.x, y}, b, p.groupBorder, 3.0f * gScale, 0, gPx);
    }
    if (textSize.x > 0) {
        dl->AddRectFilled({tx - 2.0f * gScale, a.y}, {tx + textSize.x + 2.0f * gScale, a.y + textSize.y}, p.face);
        dl->AddText({tx, a.y}, gTheme == Theme::Luna ? p.groupCaption : p.text, title);
    }
}

void selectionFill(ImDrawList* dl, ImVec2 a, ImVec2 b) { dl->AddRectFilled(snap(a), snap(b), palette().selection); }

} // namespace ui
