// Main window: menu bar, the armour workspace, the status bar, keyboard shortcuts, theme
// switching, screenshots and spall.ini. The armour panes are in armour_ui.cpp.
#include "app/app.h"

#include "app/armour_ui.h"
#include "app/settings_ini.h"
#include "core/gl.h"
#include "core/gl_util.h"
#include "core/image_io.h"
#include "core/log.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

struct AppState {
    std::filesystem::path root;
    ArmourUi armour;
    ui::Theme theme = ui::Theme::Classic;
    ui::Theme pendingTheme = ui::Theme::Classic;
    float dpiScale = 1;

    bool showLog = false, showAbout = false;
    std::string status;
    double fps = 0;
    bool screenshotRequested = false;
};

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char text[32];
    std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
    return text;
}

// ---- Menu bar ----

void mainMenu(AppState& s, GLFWwindow* window) {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ui::menuItem("Save Screenshot", "F12")) s.screenshotRequested = true;
        ImGui::Separator();
        if (ui::menuItem("Exit", "Alt+F4")) glfwSetWindowShouldClose(window, GLFW_TRUE);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (int t = 0; t < static_cast<int>(ui::Theme::Count); ++t)
            if (ui::menuItem(ui::themeName(static_cast<ui::Theme>(t)), nullptr, s.theme == static_cast<ui::Theme>(t)))
                s.pendingTheme = static_cast<ui::Theme>(t);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        if (ui::menuItem("Log", nullptr, s.showLog)) s.showLog = !s.showLog;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ui::menuItem("About Spall")) s.showAbout = true;
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

// ---- Status bar ----

void statusBar(AppState& s, float y) {
    const float sc = ui::scale();
    ImGui::SetCursorScreenPos({ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x, y});
    char text[160];
    if (!s.status.empty())
        std::snprintf(text, sizeof(text), "%s", s.status.c_str());
    else
        std::snprintf(text, sizeof(text), "Armour vs projectile: terminal ballistics of generic plates");
    const float gap = std::floor(2 * sc);
    const float pane = std::floor(ImGui::CalcTextSize("dt 0.000e-00 s").x + 12 * sc);
    ui::statusPane(text, ImGui::GetContentRegionAvail().x - (pane + gap));
    ImGui::SameLine(0, gap);
    std::snprintf(text, sizeof(text), "%.0f fps", s.fps);
    ui::statusPane(text, pane);
}

// ---- Dialogs ----

void logDialog(AppState& s) {
    if (!s.showLog) return;
    ImGui::SetNextWindowSize({std::floor(620 * ui::scale()), std::floor(320 * ui::scale())}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({std::floor(140 * ui::scale()), std::floor(200 * ui::scale())}, ImGuiCond_FirstUseEver);
    if (ui::beginWindow("Log", &s.showLog)) {
        static uint64_t seen = 0;
        const uint64_t revision = logx::revision();
        if (ui::button("Clear")) logx::clear();
        ImGui::SameLine();
        ImGui::TextDisabled("%llu entries written", static_cast<unsigned long long>(revision));
        if (ui::beginSunken("##log", {0, 0}, true, ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGui::PushFont(ui::monoFont(), 0.0f);
            for (const logx::Entry& e : logx::entries()) {
                const ImVec4 colour = e.level == logx::Level::Error  ? ui::palette().bad
                                      : e.level == logx::Level::Warn ? ui::palette().warn
                                                                     : ImGui::ColorConvertU32ToFloat4(ui::palette().text);
                ImGui::TextColored(colour, "%8.2f  %s", e.time, e.text.c_str());
            }
            if (revision != seen) ImGui::SetScrollHereY(1.0f);
            seen = revision;
            ImGui::PopFont();
        }
        ui::endSunken();
    }
    ui::endWindow();
}

void aboutDialog(AppState& s) {
    if (!s.showAbout) return;
    ImGui::SetNextWindowSize({std::floor(440 * ui::scale()), 0}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ui::beginWindow("About Spall", &s.showAbout, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushFont(ui::boldFont(), 0.0f);
        ImGui::TextUnformatted("Spall");
        ImGui::PopFont();
        ImGui::TextUnformatted("Armour vs projectile: terminal ballistics viewer");
        ImGui::Separator();
        ImGui::PushTextWrapPos(std::floor(420 * ui::scale()));
        ImGui::TextUnformatted(
            "Penetration of generic plates by long rods, compact AP shot and shaped-charge jets, from "
            "published penetration models, plus a GPU material-point simulation of the plate and "
            "projectile deforming.");
        ImGui::Spacing();
        ImGui::TextUnformatted(
            "Outputs are physical quantities only: penetration depth, residual velocity and the "
            "deformation of the metal. There are no damage, lethality, casualty or targeting outputs. "
            "Catalogue data are from open sources; each entry lists its sources and a confidence "
            "rating.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::TextDisabled("Dear ImGui %s, GLFW, OpenGL 4.6.", IMGUI_VERSION);
        ImGui::Spacing();
        const float bw = std::floor(84 * ui::scale());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
        if (ui::button("OK", {bw, 0}, true)) s.showAbout = false;
    }
    ui::endWindow();
}

// ---- Main window ----

// Lays out the whole window; `armourTop` and `armourBottom` receive the band the armour windows fill.
void mainWindow(AppState& s, GLFWwindow* window, float& armourTop, float& armourBottom) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::Begin("##main", nullptr, flags);
    mainMenu(s, window);

    const float sc = ui::scale();
    const float statusH = std::floor(ImGui::GetFontSize() + 5.0f * sc);
    const float statusY = vp->Pos.y + vp->Size.y - statusH - std::floor(3 * sc);
    armourTop = ImGui::GetCursorScreenPos().y;
    armourBottom = statusY - std::floor(4 * sc);
    statusBar(s, statusY);
    ImGui::End();
}

void shortcuts(AppState& s, GLFWwindow* window) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) s.screenshotRequested = true;
    if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F4, false)) glfwSetWindowShouldClose(window, GLFW_TRUE);
}

// ---- spall.ini ----

void applySettings(AppState& s, const UiSettings& u) {
    ui::Theme theme = ui::Theme::Classic;
    if (ui::parseTheme(u.theme.c_str(), theme)) s.theme = s.pendingTheme = theme;
    s.armour.setDisplay({u.armourDeformation, u.armourColour, u.armourQuality, u.armourSoftMounts});
}

UiSettings collectSettings(const AppState& s, const UiSettings& previous) {
    UiSettings u = previous;  // keeps the window rectangle
    u.theme = ui::themeId(s.theme);
    const ArmourUi::Display d = s.armour.display();
    u.armourDeformation = d.deformation;
    u.armourColour = d.colour;
    u.armourQuality = d.quality;
    u.armourSoftMounts = d.softMounts;
    return u;
}

// True when the window rectangle overlaps a monitor's work area by a usable amount.
bool onScreen(int x, int y, int w, int h) {
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        const int ox = std::min(x + w, mx + mw) - std::max(x, mx), oy = std::min(y + h, my + mh) - std::max(y, my);
        if (ox >= 120 && oy >= 60) return true;
    }
    return false;
}

std::filesystem::path screenshotPath(const std::filesystem::path& root) {
    return root / "screenshots" / ("spall-" + timestamp() + ".png");
}

} // namespace

int runApp(GLFWwindow* window, const std::filesystem::path& root, const AppOptions& options) {
    const bool capturing = !options.capture.empty();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 460");
    glfwSwapInterval(1);
    ui::loadFonts();

    auto state = std::make_unique<AppState>();
    AppState& s = *state;
    s.root = root;

    // Settings: spall.ini, then the command line. Captures ignore the ini so they repeat.
    const std::filesystem::path iniPath = root / "spall.ini";
    UiSettings ini;
    if (!capturing) {
        std::vector<std::string> warnings;
        loadSettings(iniPath, ini, warnings);
        for (const std::string& w : warnings) logx::warn("%s", w.c_str());
        applySettings(s, ini);
    }
    if (!options.theme.empty() && !ui::parseTheme(options.theme.c_str(), s.theme))
        logx::error("unknown theme: %s", options.theme.c_str());
    s.pendingTheme = s.theme;
    float xs = 1, ys = 1;
    glfwGetWindowContentScale(window, &xs, &ys);
    s.dpiScale = capturing ? 1.0f : xs;
    ui::setTheme(s.theme, s.dpiScale);

    if (!capturing) {
        if (ini.hasWindowRect && !options.sizeGiven && onScreen(ini.windowX, ini.windowY, ini.windowWidth, ini.windowHeight)) {
            glfwSetWindowPos(window, ini.windowX, ini.windowY);
            glfwSetWindowSize(window, ini.windowWidth, ini.windowHeight);
        } else if (!options.sizeGiven && s.dpiScale > 1.01f) {
            // First start on a high-DPI monitor: grow the default size with the scale.
            int w = 0, h = 0;
            glfwGetWindowSize(window, &w, &h);
            glfwSetWindowSize(window, static_cast<int>(w * s.dpiScale), static_cast<int>(h * s.dpiScale));
        }
        if (ini.maximized && !options.sizeGiven) glfwMaximizeWindow(window);
        glfwShowWindow(window);
    }

    s.armour.load(root);
    if (options.armourMpm || !options.mpmColour.empty() || !options.mpmQuality.empty() || options.mpmSoftMounts) {
        ArmourUi::Display d = s.armour.display();
        d.deformation = true;
        if (!options.mpmColour.empty()) {
            static const char* const names[] = {"material", "stress", "strain", "temperature", "speed", "failure"};
            const auto it = std::find(std::begin(names), std::end(names), options.mpmColour);
            if (it != std::end(names))
                d.colour = static_cast<int>(it - std::begin(names));
            else
                logx::warn("unknown --mpm-colour '%s'", options.mpmColour.c_str());
        }
        if (!options.mpmQuality.empty()) {
            static const char* const names[] = {"low", "medium", "high", "ultra", "insane", "ludicrous"};
            const auto it = std::find(std::begin(names), std::end(names), options.mpmQuality);
            if (it != std::end(names))
                d.quality = static_cast<int>(it - std::begin(names));
            else
                logx::warn("unknown --mpm-quality '%s'", options.mpmQuality.c_str());
        }
        if (options.mpmSoftMounts) d.softMounts = true;
        s.armour.setDisplay(d);
    }
    if (!options.projectile.empty() || !options.preset.empty() || options.armourAt >= 0 ||
        options.obliquity != kNoObliquity)
        s.armour.setCase(options.projectile, options.preset, options.armourAt, options.obliquity);
    if (options.prerender) s.armour.prerender();

    // --capture renders into an off-screen target (the window is hidden) with a fixed step.
    gl::Texture2D captureColour;
    gl::Framebuffer captureTarget;
    int frame = 0, exitCode = 0;
    int restoredX = ini.windowX, restoredY = ini.windowY, restoredW = ini.windowWidth, restoredH = ini.windowHeight;
    bool haveRect = ini.hasWindowRect;
    double statusSince = 0;
    std::string lastStatus;

    double last = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        const double now = glfwGetTime();
        const double dt = capturing ? 1.0 / 60.0 : now - last;
        last = now;
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        if (width <= 0 || height <= 0) {
            glfwWaitEvents();
            continue;
        }
        if (!capturing && !glfwGetWindowAttrib(window, GLFW_MAXIMIZED) && !glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            glfwGetWindowPos(window, &restoredX, &restoredY);
            glfwGetWindowSize(window, &restoredW, &restoredH);
            haveRect = true;
        }
        s.fps = s.fps <= 0 ? 1.0 / std::max(dt, 1e-3) : 0.95 * s.fps + 0.05 / std::max(dt, 1e-3);
        // Messages stay in the status bar for four seconds.
        if (s.status != lastStatus) {
            lastStatus = s.status;
            statusSince = now;
        } else if (!s.status.empty() && now - statusSince > 4.0) {
            s.status.clear();
            lastStatus.clear();
        }

        // Theme or monitor scale changes apply between frames.
        if (!capturing) glfwGetWindowContentScale(window, &xs, &ys);
        const float wantScale = capturing ? 1.0f : xs;
        if (s.pendingTheme != s.theme || std::fabs(wantScale - s.dpiScale) > 0.01f) {
            s.theme = s.pendingTheme;
            s.dpiScale = wantScale;
            ui::setTheme(s.theme, s.dpiScale);
        }

        GLuint target = 0;
        if (capturing) {
            if (captureColour.width() != width || captureColour.height() != height) {
                captureColour.allocate(width, height, GL_RGBA8);
                captureTarget.attach({&captureColour});
            }
            target = captureTarget.id();
        }
        // Shows the frame; while capturing, writes the PNG after the last frame and returns false.
        auto present = [&]() {
            if (s.screenshotRequested) {
                s.screenshotRequested = false;
                std::vector<unsigned char> rgba = gl::readPixels(target, width, height);
                for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
                const std::filesystem::path file = screenshotPath(root);
                std::error_code ec;
                std::filesystem::create_directories(file.parent_path(), ec);
                std::string error;
                if (writePng(file, width, height, rgba, error)) {
                    s.status = "Saved " + file.filename().string() + " in the screenshots folder";
                    logx::info("screenshot: %s", file.string().c_str());
                } else {
                    s.status = "Screenshot failed: " + error;
                    logx::error("screenshot: %s", error.c_str());
                }
            }
            if (!capturing) {
                glfwSwapBuffers(window);
                return true;
            }
            if (++frame < options.frames) return true;
            std::vector<unsigned char> rgba = gl::readPixels(target, width, height);
            for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
            std::string error;
            if (writePng(options.capture, width, height, rgba, error)) {
                logx::info("capture: wrote %s (%dx%d)", options.capture.string().c_str(), width, height);
            } else {
                logx::error("capture: %s", error.c_str());
                exitCode = 1;
            }
            return false;
        };

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        float armourTop = 0, armourBottom = 0;
        mainWindow(s, window, armourTop, armourBottom);
        s.armour.frame(dt, io.DisplaySize.x, armourBottom, armourTop);
        logDialog(s);
        aboutDialog(s);
        shortcuts(s, window);
        ImGui::Render();

        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glViewport(0, 0, width, height);
        const ImVec4 face = ImGui::ColorConvertU32ToFloat4(ui::palette().face);
        glClearColor(face.x, face.y, face.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (!present()) break;
    }

    if (!capturing) {
        UiSettings out = collectSettings(s, ini);
        out.hasWindowRect = haveRect;
        out.windowX = restoredX;
        out.windowY = restoredY;
        out.windowWidth = std::max(restoredW, 320);
        out.windowHeight = std::max(restoredH, 240);
        out.maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
        std::string error;
        if (!saveSettings(iniPath, out, error)) logx::warn("spall.ini: %s", error.c_str());
    }
    state.reset();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    return exitCode;
}
