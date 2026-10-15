#include "ui/widgets.h"

#include "ui/theme.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ui {
namespace {

struct Region {
    ImVec2 pos, size;
    const char* title;
};
std::vector<Region> gGroups, gSunken;

void pushNoHeader() {
    ImGui::PushStyleColor(ImGuiCol_Header, 0u);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0u);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, 0u);
}

void textCentred(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* label, ImU32 colour, ImVec2 offset = {0, 0}) {
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 size = ImGui::CalcTextSize(label, end);
    const ImVec2 pos{std::floor(0.5f * (a.x + b.x - size.x) + offset.x), std::floor(0.5f * (a.y + b.y - size.y) + offset.y)};
    dl->AddText(pos, colour, label, end);
}

// Small "x" glyph of thickness px() for close buttons.
void crossGlyph(ImDrawList* dl, ImVec2 c, float half, ImU32 colour, float thickness) {
    dl->AddLine({c.x - half, c.y - half}, {c.x + half, c.y + half}, colour, thickness);
    dl->AddLine({c.x - half, c.y + half}, {c.x + half, c.y - half}, colour, thickness);
}

void tick(ImDrawList* dl, ImVec2 a, float size, ImU32 colour) {
    const float t = std::max(1.5f, size * 0.14f);
    const ImVec2 p0{a.x + size * 0.18f, a.y + size * 0.52f}, p1{a.x + size * 0.42f, a.y + size * 0.76f},
        p2{a.x + size * 0.84f, a.y + size * 0.26f};
    dl->AddLine(p0, p1, colour, t);
    dl->AddLine(p1, p2, colour, t);
}

void textV(const ImVec4& colour, const char* fmt, va_list args) { ImGui::TextColoredV(colour, fmt, args); }

} // namespace

bool button(const char* label, ImVec2 size, bool isDefault) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 sz = ImGui::CalcItemSize(size, text.x + 2.0f * st.FramePadding.x + 8.0f * scale(),
                                          text.y + 2.0f * st.FramePadding.y + 2.0f * px());
    const bool pressed = ImGui::InvisibleButton(label, sz);
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive() && hovered;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    buttonFrame(dl, a, b, hovered, held, isDefault);
    const ImVec2 offset = held && theme() == Theme::Classic ? ImVec2{px(), px()} : ImVec2{0, 0};
    textCentred(dl, a, b, label, ImGui::GetColorU32(ImGuiCol_Text), offset);
    return pressed;
}

bool toolButton(const char* label, bool checked, ImVec2 size, const char* tooltip) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 sz = ImGui::CalcItemSize(size, text.x + 2.0f * st.FramePadding.x + 6.0f * scale(),
                                          text.y + 2.0f * st.FramePadding.y + 4.0f * px());
    const bool pressed = ImGui::InvisibleButton(label, sz);
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive() && hovered;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Palette& p = palette();
    const bool down = checked || held;
    if (theme() == Theme::Classic) {
        if (checked && !held) fillRect(dl, a, b, p.light);
        if (down) bevel(dl, a, b, Edge::ShallowSunken);
        else if (hovered) bevel(dl, a, b, Edge::ShallowRaised);
    } else {
        const float r = 3.0f * scale();
        if (down) {
            dl->AddRectFilled(a, b, withAlpha(p.selection, held ? 0.32f : 0.2f), r);
            dl->AddRect(a, b, p.selection, r, 0, px());
        } else if (hovered) {
            dl->AddRectFilled(a, b, withAlpha(p.selection, 0.1f), r);
            dl->AddRect(a, b, withAlpha(p.selection, 0.7f), r, 0, px());
        }
    }
    const ImVec2 offset = held && theme() == Theme::Classic ? ImVec2{px(), px()} : ImVec2{0, 0};
    textCentred(dl, a, b, label, ImGui::GetColorU32(ImGuiCol_Text), offset);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

bool beginCombo(const char* label, const char* preview, ImGuiComboFlags flags) {
    ImGui::PushStyleColor(ImGuiCol_PopupBg, palette().field);
    const bool open = ImGui::BeginCombo(label, preview, flags);
    ImGui::PopStyleColor();
    return open;
}

void endCombo() { ImGui::EndCombo(); }

bool comboItem(const char* label, bool selected) {
    pushNoHeader();
    ImGui::PushID(label);
    const bool clicked = ImGui::Selectable("##item", selected);
    ImGui::PopID();
    ImGui::PopStyleColor(3);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Palette& p = palette();
    if (hovered) selectionFill(dl, a, b);
    else if (selected) dl->AddRectFilled(a, b, withAlpha(p.selection, 0.18f));
    const char* end = ImGui::FindRenderedTextEnd(label);
    dl->AddText({a.x + 2.0f * scale(), a.y}, hovered ? p.textSelected : p.text, label, end);
    if (selected) ImGui::SetItemDefaultFocus();
    return clicked;
}

bool combo(const char* label, int* current, const char* const items[], int count) {
    bool changed = false;
    const char* preview = *current >= 0 && *current < count ? items[*current] : "";
    if (beginCombo(label, preview)) {
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            if (comboItem(items[i], i == *current)) {
                *current = i;
                changed = true;
            }
            ImGui::PopID();
        }
        endCombo();
    }
    return changed;
}

bool combo(const char* label, int* current, const char* zeroSeparatedItems) {
    const char* items[64];
    int count = 0;
    for (const char* s = zeroSeparatedItems; *s && count < 64; s += std::strlen(s) + 1) items[count++] = s;
    return combo(label, current, items, count);
}

bool menuItem(const char* label, const char* shortcut, bool checked, bool enabled) {
    const float fh = ImGui::GetFontSize();
    const float labelWidth = ImGui::CalcTextSize(label, nullptr, true).x;
    const float shortcutWidth = shortcut ? ImGui::CalcTextSize(shortcut).x : 0.0f;
    const float checkWidth = fh + 4.0f * scale();
    const float width = checkWidth + labelWidth + (shortcut ? shortcutWidth + 2.5f * fh : fh);
    pushNoHeader();
    ImGui::PushID(label);
    const bool clicked = ImGui::Selectable("##menu", false,
                                           (enabled ? 0 : ImGuiSelectableFlags_Disabled) | ImGuiSelectableFlags_SpanAvailWidth,
                                           {width, 0});
    ImGui::PopID();
    ImGui::PopStyleColor(3);
    const bool hovered = enabled && ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Palette& p = palette();
    if (hovered) selectionFill(dl, a, b);
    const ImU32 colour = !enabled ? p.textDisabled : hovered ? p.textSelected : p.text;
    if (checked) tick(dl, {a.x + 2.0f * scale(), a.y + 0.5f * (b.y - a.y - fh)}, fh, colour);
    dl->AddText({a.x + checkWidth, a.y}, colour, label, ImGui::FindRenderedTextEnd(label));
    if (shortcut) dl->AddText({b.x - shortcutWidth - 0.6f * fh, a.y}, colour, shortcut);
    return clicked && enabled;
}

bool beginWindow(const char* title, bool* open, ImGuiWindowFlags flags) {
    const float s = scale(), p = px();
    const bool luna = theme() == Theme::Luna;
    const float frame = luna ? std::floor(3.0f * s) : 4.0f * p;
    const float caption = std::floor(ImGui::GetFontSize() + 5.0f * s);
    const float barHeight = frame + caption;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, frame);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {frame + 4.0f * s, 4.0f * s});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.0f * s, 0.5f * (barHeight - ImGui::GetFontSize())});
    ImGui::PushStyleColor(ImGuiCol_Border, 0u);
    const bool visible = ImGui::Begin(title, nullptr, flags | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                                          ImGuiWindowFlags_MenuBar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    const ImVec2 end{pos.x + size.x, pos.y + size.y};
    const bool active = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Palette& pal = palette();
    dl->PushClipRect(pos, end, false);
    if (luna) {
        const ImU32 c = active ? pal.captionActive[0] : pal.captionInactive[0];
        fillRect(dl, pos, {pos.x + frame, end.y}, c);
        fillRect(dl, {end.x - frame, pos.y}, end, c);
        fillRect(dl, {pos.x, end.y - frame}, end, c);
        dl->AddRect(pos, end, active ? IM_COL32(0x08, 0x31, 0xD9, 255) : IM_COL32(0x6F, 0x8A, 0xD3, 255), 0.0f, 0, p);
    } else {
        bevel(dl, pos, end, Edge::Raised);
    }
    ImVec2 ca{pos.x + frame, pos.y + (luna ? 0.0f : frame)}, cb{end.x - frame, pos.y + barHeight};
    if (luna) {
        ca.x = pos.x;
        cb.x = end.x;
    }
    captionBar(dl, ca, cb, title, active, luna);
    dl->PopClipRect();
    if (open && ImGui::BeginMenuBar()) {
        const float bs = std::floor(caption - 4.0f * s);
        const ImVec2 ba{std::floor(cb.x - (luna ? frame : 2.0f * s) - bs - (luna ? 2.0f * s : 0.0f)), std::floor(cb.y - caption + 2.0f * s)};
        const ImVec2 bb{ba.x + bs, ba.y + bs};
        ImGui::SetCursorScreenPos(ba);
        ImGui::PushClipRect(pos, end, false);
        if (ImGui::InvisibleButton("##close", {bs, bs})) *open = false;
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive() && hovered;
        ImGui::PopClipRect();
        dl->PushClipRect(pos, end, false);
        const ImVec2 c{std::floor(0.5f * (ba.x + bb.x)) + (held && !luna ? p : 0.0f), std::floor(0.5f * (ba.y + bb.y)) + (held && !luna ? p : 0.0f)};
        if (luna) {
            const float r = 3.0f * s;
            const ImU32 top = hovered ? IM_COL32(0xF0, 0x8C, 0x70, 255) : IM_COL32(0xE4, 0x6C, 0x4A, 255);
            const ImU32 bottom = held ? IM_COL32(0xA8, 0x2C, 0x0C, 255) : IM_COL32(0xC4, 0x3A, 0x18, 255);
            dl->AddRectFilled(ba, bb, bottom, r);
            dl->AddRectFilledMultiColor({ba.x + 2 * p, ba.y + 2 * p}, {bb.x - 2 * p, bb.y - 2 * p}, top, top, bottom, bottom);
            dl->AddRect(ba, bb, IM_COL32_WHITE, r, 0, p);
            crossGlyph(dl, c, std::floor(bs * 0.22f), IM_COL32_WHITE, 2.0f * p);
        } else {
            buttonFrame(dl, ba, bb, hovered, held);
            crossGlyph(dl, {c.x - 0.5f, c.y - 0.5f}, std::floor(bs * 0.2f), pal.text, 1.5f * p);
        }
        dl->PopClipRect();
        ImGui::EndMenuBar();
    }
    return visible;
}

void endWindow() { ImGui::End(); }

void beginGroupBox(const char* title) {
    const float s = scale();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    gGroups.push_back({pos, {width, 0}, title});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.0f * s, ImGui::GetFontSize() + 3.0f * s});
    ImGui::BeginChild(title, {width, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
}

void endGroupBox() {
    ImGui::EndChild();
    const Region r = gGroups.back();
    gGroups.pop_back();
    groupFrame(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), r.title);
}

bool beginSunken(const char* id, ImVec2 size, bool field, ImGuiWindowFlags flags) {
    const ImVec2 pos = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    if (size.x <= 0) size.x = std::max(avail.x + size.x, 8.0f);
    if (size.y <= 0) size.y = std::max(avail.y + size.y, 8.0f);
    size = {std::floor(size.x), std::floor(size.y)};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (field) fillRect(dl, pos, {pos.x + size.x, pos.y + size.y}, palette().field);
    bevel(dl, pos, {pos.x + size.x, pos.y + size.y}, Edge::Sunken);
    gSunken.push_back({pos, size, nullptr});
    const float inset = theme() == Theme::Classic ? 2.0f * px() : px();
    ImGui::BeginGroup();
    ImGui::SetCursorScreenPos({pos.x + inset, pos.y + inset});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, 0u);
    const bool visible = ImGui::BeginChild(id, {size.x - 2 * inset, size.y - 2 * inset}, ImGuiChildFlags_None, flags);
    ImGui::PopStyleColor();
    return visible;
}

void endSunken() {
    ImGui::EndChild();
    const Region r = gSunken.back();
    gSunken.pop_back();
    ImGui::SetCursorScreenPos(r.pos);
    ImGui::Dummy(r.size);
    ImGui::EndGroup();
}

bool trackBar(const char* id, double* value, double lo, double hi, float width, int ticks, const double* markers,
              int markerCount) {
    const float s = scale(), p = px();
    const float height = std::floor(26.0f * s);
    const float thumbW = std::floor(11.0f * s), thumbH = std::floor(19.0f * s);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (width <= 0) width = ImGui::GetContentRegionAvail().x + width;
    width = std::max(width, 4.0f * thumbW);
    ImGui::InvisibleButton(id, {width, height});
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const float x0 = std::floor(pos.x + 0.5f * thumbW + 2.0f * s), x1 = std::floor(pos.x + width - 0.5f * thumbW - 2.0f * s);
    bool changed = false;
    const double span = hi > lo ? hi - lo : 1.0;
    if (active) {
        const float mx = ImGui::GetIO().MousePos.x;
        const double v = lo + span * std::clamp((mx - x0) / std::max(x1 - x0, 1.0f), 0.0f, 1.0f);
        if (v != *value) {
            *value = v;
            changed = true;
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Palette& pal = palette();
    const float cy = std::floor(pos.y + 8.0f * s);
    // Channel.
    if (theme() == Theme::Classic) {
        bevel(dl, {x0 - 2 * p, cy - 2 * p}, {x1 + 2 * p, cy + 2 * p}, Edge::Sunken);
    } else {
        dl->AddRectFilled({x0 - 2 * p, cy - 2 * p}, {x1 + 2 * p, cy + 2 * p}, IM_COL32(0xF4, 0xF3, 0xEE, 255), 2.0f * s);
        dl->AddRect({x0 - 2 * p, cy - 2 * p}, {x1 + 2 * p, cy + 2 * p}, IM_COL32(0x9D, 0x9C, 0x99, 255), 2.0f * s, 0, p);
    }
    // Ticks below the thumb.
    const float ty = std::floor(pos.y + height - 4.0f * s);
    for (int i = 0; i <= ticks && ticks > 0; ++i) {
        const float x = std::floor(x0 + (x1 - x0) * i / ticks);
        dl->AddRectFilled({x, ty}, {x + p, ty + 3.0f * s}, pal.text);
    }
    // Markers (stored checkpoints) above the channel.
    for (int i = 0; i < markerCount; ++i) {
        const float x = std::floor(x0 + (x1 - x0) * static_cast<float>((markers[i] - lo) / span));
        dl->AddTriangleFilled({x - 3.0f * s, pos.y}, {x + 3.0f * s, pos.y}, {x, pos.y + 4.0f * s},
                              ImGui::ColorConvertFloat4ToU32(pal.accent));
    }
    // Thumb: a raised block with a pointed bottom.
    const float tx = std::floor(x0 + (x1 - x0) * static_cast<float>(std::clamp((*value - lo) / span, 0.0, 1.0)));
    const ImVec2 ta{tx - std::floor(0.5f * thumbW), cy - std::floor(0.45f * thumbH)};
    const ImVec2 tb{ta.x + thumbW, ta.y + thumbH - std::floor(4.0f * s)};
    if (theme() == Theme::Classic) {
        fillRect(dl, ta, {tb.x, tb.y + 4.0f * s}, pal.face);
        bevel(dl, ta, tb, Edge::Raised);
        dl->AddTriangleFilled({ta.x, tb.y}, {tb.x, tb.y}, {tx, tb.y + 4.0f * s}, pal.face);
        dl->AddLine({ta.x + 0.5f, tb.y - 0.5f}, {tx + 0.5f, tb.y + 4.0f * s}, pal.highlight, p);
        dl->AddLine({tb.x - 0.5f, tb.y - 0.5f}, {tx + 0.5f, tb.y + 4.0f * s}, pal.dark, p);
    } else {
        const ImU32 top = IM_COL32_WHITE, bottom = active ? IM_COL32(0xD8, 0xD6, 0xC8, 255) : IM_COL32(0xEC, 0xEB, 0xE6, 255);
        dl->AddRectFilledMultiColor(ta, tb, top, top, bottom, bottom);
        dl->AddRect(ta, tb, hovered || active ? pal.hoverRim : IM_COL32(0x1C, 0x51, 0x80, 255), 2.0f * s, 0, p);
        dl->AddRectFilled({ta.x + 2 * p, tb.y - 3.0f * s}, {tb.x - 2 * p, tb.y - p}, IM_COL32(0x21, 0xA1, 0x21, 255));
    }
    return changed;
}

void progressBar(float fraction, ImVec2 size) {
    const float s = scale(), p = px();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    size = ImGui::CalcItemSize(size, ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() + 4.0f * s);
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end{pos.x + size.x, pos.y + size.y};
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    const bool luna = theme() == Theme::Luna;
    if (luna) {
        dl->AddRectFilled(pos, end, IM_COL32_WHITE, 3.0f * s);
        dl->AddRect(pos, end, IM_COL32(0x68, 0x68, 0x68, 255), 3.0f * s, 0, p);
    } else {
        fillRect(dl, pos, end, palette().face);
        bevel(dl, pos, end, Edge::ShallowSunken);
    }
    const float inset = (luna ? 3.0f : 2.0f) * p;
    const float x0 = pos.x + inset, x1 = end.x - inset, y0 = pos.y + inset, y1 = end.y - inset;
    const float block = std::floor(luna ? 6.0f * s : 0.6f * (y1 - y0)), gap = std::floor(2.0f * s);
    const float fill = x0 + (x1 - x0) * fraction;
    for (float x = x0; x + 1.0f < fill; x += block + gap) {
        const float xe = std::min(x + block, x1);
        if (luna) {
            const ImU32 light = IM_COL32(0xB8, 0xF2, 0xB0, 255), green = palette().progress;
            const float ym = std::floor(0.5f * (y0 + y1));
            dl->AddRectFilledMultiColor({x, y0}, {xe, ym}, light, light, green, green);
            dl->AddRectFilledMultiColor({x, ym}, {xe, y1}, green, green, light, light);
        } else {
            dl->AddRectFilled({std::floor(x), y0}, {std::floor(xe), y1}, palette().selection);
        }
    }
}

void statusPane(const char* text, float width) {
    const float s = scale();
    if (width <= 0) width = ImGui::GetContentRegionAvail().x;
    const float height = std::floor(ImGui::GetFontSize() + 5.0f * s);
    const ImVec2 pos = ImGui::GetCursorScreenPos(), end{pos.x + width, pos.y + height};
    ImGui::Dummy({width, height});
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bevel(dl, pos, end, Edge::ShallowSunken);
    dl->PushClipRect({pos.x + px(), pos.y + px()}, {end.x - px(), end.y - px()}, true);
    dl->AddText({std::floor(pos.x + 4.0f * s), std::floor(pos.y + 0.5f * (height - ImGui::GetFontSize()))},
                ImGui::GetColorU32(ImGuiCol_Text), text);
    dl->PopClipRect();
}

bool beginPropertyGrid(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInner | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp))
        return false;
    ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
    return true;
}

void propertySection(const char* title) {
    ImGui::TableNextRow();
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, palette().face);
    ImGui::TableSetColumnIndex(0);
    ImGui::PushFont(boldFont(), 0.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
}

void property(const char* name, const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(name);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextWrapped("%s", buffer);
}

void endPropertyGrid() { ImGui::EndTable(); }

void tableHeaders() {
    ImGuiTable* table = ImGui::GetCurrentTable();
    if (!table) return;
    const int columns = ImGui::TableGetColumnCount();
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int i = 0; i < columns; ++i) {
        if (!ImGui::TableSetColumnIndex(i)) continue;
        ImGui::PushID(i);
        ImGui::TableHeader(ImGui::TableGetColumnName(i));
        ImGui::PopID();
        const ImRect r = ImGui::TableGetCellBgRect(table, i);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (theme() == Theme::Classic) {
            bevel(dl, r.Min, r.Max, Edge::Raised);
        } else {
            dl->AddRectFilled({r.Min.x, r.Max.y - 3.0f * px()}, r.Max, IM_COL32(0xE2, 0xDE, 0xCD, 255));
            dl->AddRectFilled({r.Max.x - px(), r.Min.y + 3.0f * scale()}, {r.Max.x, r.Max.y - 4.0f * scale()}, palette().shadow);
        }
    }
}

bool splitter(const char* id, float* size, float lo, float hi, float height, bool fromRight) {
    const float thickness = std::floor(5.0f * scale());
    ImGui::InvisibleButton(id, {thickness, std::max(height, 1.0f)});
    const bool active = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (active) {
        const float delta = ImGui::GetIO().MouseDelta.x;
        *size = std::clamp(*size + (fromRight ? -delta : delta), lo, std::max(lo, hi));
    }
    return active;
}

void numberText(const char* fmt, ...) {
    ImGui::PushFont(monoFont(), 0.0f);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
    ImGui::PopFont();
}

void goodText(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    textV(palette().good, fmt, args);
    va_end(args);
}

void warnText(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    textV(palette().warn, fmt, args);
    va_end(args);
}

void badText(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    textV(palette().bad, fmt, args);
    va_end(args);
}

} // namespace ui
