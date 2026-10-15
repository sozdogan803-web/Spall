// Themed widgets built on Dear ImGui and the ui/theme draw primitives: push and tool buttons,
// combo and menu items with selection highlighting, captioned windows, group boxes, sunken
// panes, a track bar with tick marks, progress bar, status panes, property grid, list-view
// headers and column splitters.
#pragma once

#include "imgui.h"

namespace ui {

// Push button; a zero size fits the label.
bool button(const char* label, ImVec2 size = {0, 0}, bool isDefault = false);
// Flat tool button: raised when hovered, sunken while pressed or `checked`.
bool toolButton(const char* label, bool checked, ImVec2 size = {0, 0}, const char* tooltip = nullptr);

// Combo box whose drop-down items highlight with the selection colours.
bool beginCombo(const char* label, const char* preview, ImGuiComboFlags flags = 0);
void endCombo();
bool comboItem(const char* label, bool selected);
bool combo(const char* label, int* current, const char* const items[], int count);
// Items separated by '\0' and terminated by "\0\0", as for ImGui::Combo.
bool combo(const char* label, int* current, const char* zeroSeparatedItems);

// Menu item drawn with the selection colours; `checked` shows a tick.
bool menuItem(const char* label, const char* shortcut = nullptr, bool checked = false, bool enabled = true);

// Floating window with a themed frame and caption bar (and a close button when `open` is set).
// Call endWindow() whatever the return value.
bool beginWindow(const char* title, bool* open = nullptr, ImGuiWindowFlags flags = 0);
void endWindow();

// Group box spanning the available width; its height follows the contents.
void beginGroupBox(const char* title);
void endGroupBox();

// Child region inside a sunken frame (list views, logs). A size component <= 0 means the
// available space plus that value. `field` paints the field (white) background.
bool beginSunken(const char* id, ImVec2 size, bool field = true, ImGuiWindowFlags flags = 0);
void endSunken();

// Horizontal track bar from lo to hi with `ticks` tick intervals and markers (values) drawn
// above the channel. Returns true while the value changes.
bool trackBar(const char* id, double* value, double lo, double hi, float width, int ticks, const double* markers,
              int markerCount);

// Progress bar: navy blocks (Classic) or green segments (Luna).
void progressBar(float fraction, ImVec2 size);

// Status-bar pane with sunken edges; width <= 0 takes the rest of the line.
void statusPane(const char* text, float width);

// Two-column property grid.
bool beginPropertyGrid(const char* id);
void propertySection(const char* title);
void property(const char* name, const char* fmt, ...) IM_FMTARGS(2);
void endPropertyGrid();

// Header row of the current table drawn as raised list-view column headers.
void tableHeaders();

// Vertical splitter of `height` between columns. Dragging changes *size by the mouse movement
// (negated for `fromRight`), clamped to [lo, hi]. Returns true while dragging.
bool splitter(const char* id, float* size, float lo, float hi, float height, bool fromRight);

// Text in the numeric (Lucida Console) font.
void numberText(const char* fmt, ...) IM_FMTARGS(1);
// Text in a palette colour for semantic states.
void goodText(const char* fmt, ...) IM_FMTARGS(1);
void warnText(const char* fmt, ...) IM_FMTARGS(1);
void badText(const char* fmt, ...) IM_FMTARGS(1);

} // namespace ui
