// Shared helpers for the JSON catalogue loaders (explosive, projectile and material databases).
#pragma once

#include "core/json.h"
#include "data/explosive_db.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace catalogue {

// Reads typed fields from one catalogue entry and remembers the first problem.
class EntryReader {
public:
    EntryReader(const json::Value& entry) : entry_(entry) {}

    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }
    void fail(const std::string& field, const std::string& problem) {
        if (error_.empty()) error_ = field + ": " + problem;
    }

    bool has(const char* key) const { return entry_.find(key) != nullptr; }

    std::string str(const char* key, bool required = true) {
        const json::Value* v = entry_.find(key);
        if (!v) {
            if (required) fail(key, "missing");
            return {};
        }
        if (!v->isString()) fail(key, "must be a string");
        return v->asString();
    }

    double num(const char* key) {
        const json::Value* v = entry_.find(key);
        if (!v) {
            fail(key, "missing");
            return 0;
        }
        if (!v->isNumber()) fail(key, "must be a number");
        return v->asNumber();
    }

    // Number that must lie in [lo, hi].
    double range(const char* key, double lo, double hi) {
        const double value = num(key);
        if (ok() && !(value >= lo && value <= hi)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%g is outside %g..%g", value, lo, hi);
            fail(key, buf);
        }
        return value;
    }

    // Optional number in [lo, hi]; `def` when absent.
    double optRange(const char* key, double lo, double hi, double def) { return has(key) ? range(key, lo, hi) : def; }

    // Optional boolean; `def` when absent.
    bool flag(const char* key, bool def) {
        const json::Value* v = entry_.find(key);
        if (!v) return def;
        if (!v->isBool()) fail(key, "must be true or false");
        return v->asBool(def);
    }

    std::vector<std::string> strings(const char* key) {
        std::vector<std::string> out;
        const json::Value* v = entry_.find(key);
        if (!v) {
            fail(key, "missing");
            return out;
        }
        if (!v->isArray()) fail(key, "must be an array of strings");
        for (const json::Value& item : v->asArray()) {
            if (!item.isString()) fail(key, "must be an array of strings");
            out.push_back(item.asString());
        }
        return out;
    }

    Confidence confidence() {
        const std::string c = str("confidence");
        if (c == "high") return Confidence::High;
        if (c == "medium") return Confidence::Medium;
        if (c == "low") return Confidence::Low;
        if (ok()) fail("confidence", "'" + c + "' is not high, medium or low");
        return Confidence::Low;
    }

private:
    const json::Value& entry_;
    std::string error_;
};

inline bool parseDocument(std::string_view text, const char* file, const char* arrayKey, json::Value& doc,
                   const json::Value*& items, std::string& fatal) {
    std::string error;
    if (!json::parse(text, doc, error)) {
        fatal = std::string(file) + ": " + error;
        return false;
    }
    const json::Value* schema = doc.find("schema");
    if (!schema || schema->asNumber(-1) != 1) {
        fatal = std::string(file) + ": unsupported or missing \"schema\" (expected 1)";
        return false;
    }
    items = doc.find(arrayKey);
    if (!items || !items->isArray()) {
        fatal = std::string(file) + ": missing \"" + arrayKey + "\" array";
        return false;
    }
    return true;
}

inline std::string entryName(const json::Value& entry, size_t index) {
    const json::Value* id = entry.find("id");
    if (id && id->isString() && !id->asString().empty()) return id->asString();
    return "#" + std::to_string(index);
}

} // namespace catalogue
