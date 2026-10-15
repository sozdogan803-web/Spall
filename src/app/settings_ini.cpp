#include "app/settings_ini.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <type_traits>
#include <sstream>

namespace {

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

// Applies one key = value line when the key matches.
struct Reader {
    const std::string& key;
    const std::string& value;
    std::vector<std::string>& warnings;
    bool matched = false;
    bool applied = false;

    bool is(const char* k) {
        if (matched || key != k) return false;
        matched = true;
        return true;
    }
    void bad(const char* what) { warnings.push_back("spall.ini: " + key + " = " + value + ": " + what); }

    template <class T>
    void number(const char* k, T& out, double lo, double hi) {
        if (!is(k)) return;
        char* end = nullptr;
        errno = 0;
        const double v = std::strtod(value.c_str(), &end);
        if (end == value.c_str() || *end != '\0' || errno != 0 || !std::isfinite(v)) return bad("not a number");
        if (v < lo || v > hi) return bad("out of range");
        if constexpr (std::is_integral_v<T>) {
            if (v != std::floor(v)) return bad("not an integer");
        }
        out = static_cast<T>(v);
        applied = true;
    }
    void flag(const char* k, bool& out) {
        if (!is(k)) return;
        if (value == "1" || value == "true" || value == "yes") out = true;
        else if (value == "0" || value == "false" || value == "no") out = false;
        else return bad("expected true or false");
        applied = true;
    }
    void text(const char* k, std::string& out, size_t maxLength) {
        if (!is(k)) return;
        if (value.size() > maxLength) return bad("too long");
        out = value;
        applied = true;
    }
};

struct Writer {
    std::ostringstream out;
    template <class T>
    void number(const char* k, T v, double, double) {
        char buf[64];
        if constexpr (std::is_integral_v<T>) std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(v));
        else if constexpr (std::is_same_v<T, float>) std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
        else std::snprintf(buf, sizeof(buf), "%.17g", static_cast<double>(v));
        out << k << " = " << buf << "\n";
    }
    void flag(const char* k, bool v) { out << k << " = " << (v ? "true" : "false") << "\n"; }
    void text(const char* k, const std::string& v, size_t) { out << k << " = " << v << "\n"; }
    void section(const char* name) { out << "\n[" << name << "]\n"; }
};

// The single list of stored values, shared by the reader and the writer. Keys are prefixed with
// their section so the file stays readable without the parser needing sections.
template <class V>
void visit(UiSettings& s, V& v) {
    v.text("ui.theme", s.theme, 32);
    v.flag("ui.window_stored", s.hasWindowRect);
    v.number("ui.window_x", s.windowX, -32768, 32767);
    v.number("ui.window_y", s.windowY, -32768, 32767);
    v.number("ui.window_width", s.windowWidth, 320, 16384);
    v.number("ui.window_height", s.windowHeight, 240, 16384);
    v.flag("ui.maximized", s.maximized);
    v.flag("armour.deformation", s.armourDeformation);
    v.number("armour.colour", s.armourColour, 0, 5);
    v.number("armour.quality", s.armourQuality, 0, 5);
    v.flag("armour.soft_mounts", s.armourSoftMounts);
}

} // namespace

bool UiSettings::operator==(const UiSettings& o) const { return formatSettings(*this) == formatSettings(o); }

int parseSettings(const std::string& text, UiSettings& out, std::vector<std::string>& warnings) {
    std::istringstream in(text);
    std::string line;
    int applied = 0;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            warnings.push_back("spall.ini: ignored line \"" + line + "\"");
            continue;
        }
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        Reader r{key, value, warnings};
        visit(out, r);
        if (!r.matched) warnings.push_back("spall.ini: unknown key " + key);
        if (r.applied) ++applied;
    }
    return applied;
}

std::string formatSettings(const UiSettings& s) {
    UiSettings copy = s;
    Writer w;
    w.out << "# Spall settings, written on exit. Delete this file to restore the defaults.\n";
    visit(copy, w);
    return w.out.str();
}

void loadSettings(const std::filesystem::path& file, UiSettings& out, std::vector<std::string>& warnings) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return;
    std::ostringstream text;
    text << in.rdbuf();
    parseSettings(text.str(), out, warnings);
}

bool saveSettings(const std::filesystem::path& file, const UiSettings& s, std::string& error) {
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + tmp.string();
            return false;
        }
        out << formatSettings(s);
        if (!out) {
            error = "write failed: " + tmp.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        error = "cannot replace " + file.string() + ": " + ec.message();
        return false;
    }
    return true;
}
