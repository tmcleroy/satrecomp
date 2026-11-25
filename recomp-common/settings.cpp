// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
#include "settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace recomp {

namespace {

    std::string Trimmed(const std::string &text) {
        const size_t first = text.find_first_not_of(" \t\r");
        if (first == std::string::npos) {
            return "";
        }
        const size_t last = text.find_last_not_of(" \t\r");
        return text.substr(first, last - first + 1);
    }

    bool SectionLine(const std::string &line, std::string &name) {
        const std::string t = Trimmed(line);
        if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
            name = Trimmed(t.substr(1, t.size() - 2));
            return true;
        }
        return false;
    }

    bool KeyLine(const std::string &line, std::string &key, std::string &value) {
        const std::string t = Trimmed(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') {
            return false;
        }
        const size_t equals = t.find('=');
        if (equals == std::string::npos) {
            return false;
        }
        key = Trimmed(t.substr(0, equals));
        value = Trimmed(t.substr(equals + 1));
        return true;
    }

    const char kSection[] = "features";

    // A feature kept by name (FeatureInfo::ids), its name_id key, and its ids and their old numbers
    bool ById(const FeatureInfo &feature) {
        return !feature.ids.empty() && feature.ids.size() == feature.choices.size();
    }

    std::string IdKey(const FeatureInfo &feature) {
        return feature.name + "_id";
    }

    // The choice an id names, or -1
    int ChoiceOfId(const FeatureInfo &feature, const std::string &id) {
        const auto at = std::find(feature.ids.begin(), feature.ids.end(), id);
        return id.empty() || at == feature.ids.end() ? -1 : static_cast<int>(at - feature.ids.begin());
    }

    const std::vector<std::string> &NumberedOrder(const FeatureInfo &feature) {
        return feature.numbered.empty() ? feature.ids : feature.numbered;
    }

    // The id an old number names ("" for none), and an id's old number (-1 for none)
    std::string IdOfNumber(const FeatureInfo &feature, long number) {
        const std::vector<std::string> &order = NumberedOrder(feature);
        return number >= 0 && number < static_cast<long>(order.size()) ? order[static_cast<size_t>(number)] : "";
    }

    int NumberOfId(const FeatureInfo &feature, const std::string &id) {
        const std::vector<std::string> &order = NumberedOrder(feature);
        const auto at = std::find(order.begin(), order.end(), id);
        return id.empty() || at == order.end() ? -1 : static_cast<int>(at - order.begin());
    }

    // A value as the file has it: 0 for off, otherwise its number (1 at least)
    int NumberIn(const std::string &text) {
        return SettingOff(text) ? 0 : std::max(std::atoi(text.c_str()), 1);
    }

    // Of a feature kept by name, the choice the file gives it: name_id's, or (none, or an id this build does not
    // know) name's number in the old order; -1 for none at all (as the preset has it)
    int StoredChoice(const Ini &ini, const FeatureInfo &feature) {
        const std::string id = ini.get(kSection, IdKey(feature));
        if (!id.empty()) {
            if (SettingOff(id)) {
                return 0;
            }
            if (const int choice = ChoiceOfId(feature, id); choice >= 0) {
                return choice;
            }
        }
        const std::string text = ini.get(kSection, feature.name);
        if (text.empty()) {
            return id.empty() ? -1 : 0; // (an id it does not know, with no number: a choice it has not)
        }
        return std::max(ChoiceOfId(feature, IdOfNumber(feature, NumberIn(text))), 0);
    }

    // Sets a key; whether it changed (none set to "" stays none)
    bool Put(Ini &ini, const std::string &section, const std::string &key, const std::string &value) {
        if (ini.get(section, key) == value) {
            return false;
        }
        ini.set(section, key, value);
        return true;
    }

    // Writes a feature's value: the number, or of a feature kept by name its id and its old number ("" for none)
    void Write(Ini &ini, const FeatureInfo &feature, int value) {
        if (!ById(feature)) {
            ini.set(kSection, feature.name, std::to_string(value));
            return;
        }
        const std::string &id = feature.ids[value >= 0 && value < static_cast<int>(feature.ids.size()) ? static_cast<size_t>(value) : 0];
        const int number = NumberOfId(feature, id);
        Put(ini, kSection, IdKey(feature), id);
        Put(ini, kSection, feature.name, number >= 0 ? std::to_string(number) : "");
    }

    bool HasValue(const Ini &ini, const FeatureInfo &feature) {
        return !ini.get(kSection, feature.name).empty() || (ById(feature) && !ini.get(kSection, IdKey(feature)).empty());
    }

    // What the file says a feature is, or what the preset makes it
    int ValueOf(const Ini &ini, const FeatureInfo &feature, bool enhanced) {
        if (feature.flags & kFeatureSession) {
            return 0;
        }
        int value = enhanced && (feature.flags & kFeatureEnhanced) ? 1 : 0;
        if (ById(feature)) {
            if (const int choice = StoredChoice(ini, feature); choice >= 0) {
                value = choice;
            }
        } else if (const std::string text = ini.get(kSection, feature.name); !text.empty()) {
            value = NumberIn(text);
        }
        if (feature.choices.empty()) {
            return value != 0;
        }
        return value < static_cast<int>(feature.choices.size()) ? value : 0;
    }

    // [menu]'s lists of the choices offered, for these: by name (name_id) and by old number (name); both "" when
    // every one is
    void MenuTexts(const FeatureInfo &feature, const std::vector<bool> &offered, std::string &ids, std::string &numbers) {
        ids.clear();
        numbers.clear();
        bool all = true;
        std::vector<int> old;
        for (size_t choice = 1; choice < feature.choices.size(); choice++) {
            if (choice < offered.size() && offered[choice]) {
                if (ById(feature)) {
                    ids += (ids.empty() ? "" : ",") + feature.ids[choice];
                    if (const int number = NumberOfId(feature, feature.ids[choice]); number > 0) {
                        old.push_back(number);
                    }
                } else {
                    old.push_back(static_cast<int>(choice));
                }
            } else {
                all = false;
            }
        }
        if (all) {
            ids.clear();
            return;
        }
        std::sort(old.begin(), old.end());
        for (int number : old) {
            numbers += (numbers.empty() ? "" : ",") + std::to_string(number);
        }
        // (none but off: "0", which offers off alone; by name, off's id)
        if (numbers.empty()) {
            numbers = "0";
        }
        if (ById(feature) && ids.empty()) {
            ids = feature.ids[0];
        }
    }

    // Whether every name in a [menu] list by name is one of the feature's
    bool KnownIds(const FeatureInfo &feature, const std::string &list) {
        std::istringstream in(list);
        for (std::string id; std::getline(in, id, ',');) {
            if (ChoiceOfId(feature, Trimmed(id)) < 0) {
                return false;
            }
        }
        return true;
    }

    bool Exclusive(const FeatureInfo &a, const FeatureInfo &b) {
        return (a.flags & kFeatureExclusive) && (b.flags & kFeatureExclusive) && a.category == b.category;
    }

} // namespace

Ini::Ini(std::filesystem::path path) : path_(std::move(path)) {}

Ini::Ini(const std::string &path)
#ifdef __cpp_char8_t
    : path_(std::u8string(path.begin(), path.end())) {}
#else
    : path_(std::filesystem::u8path(path)) {}
#endif

void Ini::load() {
    lines_.clear();
    std::ifstream file(path_, std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines_.push_back(line);
    }
}

bool Ini::save() const {
    std::error_code error;
    if (path_.has_parent_path()) {
        std::filesystem::create_directories(path_.parent_path(), error);
    }
    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    for (const std::string &line : lines_) {
        file << line << '\n';
    }
    return static_cast<bool>(file);
}

std::string Ini::get(const std::string &section, const std::string &key) const {
    std::string current;
    for (const std::string &line : lines_) {
        std::string name, k, v;
        if (SectionLine(line, name)) {
            current = name;
        } else if (current == section && KeyLine(line, k, v) && k == key) {
            return v;
        }
    }
    return "";
}

void Ini::set(const std::string &section, const std::string &key, const std::string &value) {
    std::string current;
    size_t sectionEnd = std::string::npos; // where to put a new key of a section that is there
    for (size_t i = 0; i < lines_.size(); i++) {
        std::string name, k, v;
        if (SectionLine(lines_[i], name)) {
            current = name;
            continue;
        }
        if (current != section) {
            continue;
        }
        sectionEnd = i + 1;
        if (KeyLine(lines_[i], k, v) && k == key) {
            lines_[i] = key + "=" + value;
            return;
        }
    }
    if (sectionEnd == std::string::npos) {
        for (size_t i = 0; i < lines_.size(); i++) {
            std::string name;
            if (SectionLine(lines_[i], name) && name == section) {
                sectionEnd = i + 1;
            }
        }
    }
    if (sectionEnd == std::string::npos) {
        if (!lines_.empty() && !lines_.back().empty()) {
            lines_.push_back("");
        }
        lines_.push_back("[" + section + "]");
        sectionEnd = lines_.size();
    }
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(sectionEnd), key + "=" + value);
}

std::string FeatureListText(const std::vector<FeatureInfo> &features) {
    std::string text;
    for (const FeatureInfo &feature : features) {
        if (feature.flags & kFeatureSession) {
            continue;
        }
        text += feature.name + "\t" + feature.category + "\t" + std::to_string(feature.flags) + "\t" + feature.title + "\t" + feature.description;
        for (size_t i = 0; i < feature.choices.size(); i++) {
            text += (i ? "|" : "\t") + feature.choices[i];
        }
        if (ById(feature)) {
            for (size_t i = 0; i < feature.ids.size(); i++) {
                text += (i ? "|" : "\t") + feature.ids[i];
            }
            text += "\t";
            for (size_t i = 0; i < feature.numbered.size(); i++) {
                text += (i ? "|" : "") + feature.numbered[i];
            }
        }
        text += "\n";
    }
    return text;
}

std::vector<FeatureInfo> ParseFeatureList(const std::string &text) {
    std::vector<FeatureInfo> features;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // (every field, an empty one too: a feature may have no description)
        std::vector<std::string> parts;
        std::string part;
        for (size_t at = 0;;) {
            const size_t tab = line.find('\t', at);
            parts.push_back(line.substr(at, tab == std::string::npos ? tab : tab - at));
            if (tab == std::string::npos) {
                break;
            }
            at = tab + 1;
        }
        if (parts.size() < 5) {
            continue;
        }
        FeatureInfo feature;
        feature.name = parts[0];
        feature.category = parts[1];
        feature.flags = static_cast<unsigned>(std::strtoul(parts[2].c_str(), nullptr, 10));
        feature.title = parts[3];
        feature.description = parts[4];
        if (parts.size() > 5 && !parts[5].empty()) {
            std::stringstream choices(parts[5]);
            while (std::getline(choices, part, '|')) {
                feature.choices.push_back(part);
            }
        }
        if (parts.size() > 6 && !parts[6].empty()) {
            std::stringstream ids(parts[6]);
            while (std::getline(ids, part, '|')) {
                feature.ids.push_back(part);
            }
        }
        if (parts.size() > 7 && !parts[7].empty()) {
            // (every entry, an empty one too: a number that names none now)
            for (size_t at = 0;;) {
                const size_t bar = parts[7].find('|', at);
                feature.numbered.push_back(parts[7].substr(at, bar == std::string::npos ? bar : bar - at));
                if (bar == std::string::npos) {
                    break;
                }
                at = bar + 1;
            }
        }
        features.push_back(feature);
    }
    return features;
}

std::string FeatureListNote(const std::string &note) {
    std::string line = note;
    for (char &c : line) {
        if (c == '\t' || c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return "!note\t" + line + "\n";
}

std::string ParseFeatureListNote(const std::string &text) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("!note\t", 0) == 0) {
            return line.substr(6);
        }
    }
    return "";
}

bool SettingOff(const std::string &value) {
    return value == "0" || value == "off" || value == "false" || value == "no";
}

std::string Preset(const Ini &ini) {
    const std::string value = ini.get(kSection, "preset");
    return value.empty() ? "pure" : value;
}

std::vector<int> FeatureValues(const Ini &ini, const std::vector<FeatureInfo> &features) {
    const bool enhanced = Preset(ini) == "enhanced";
    std::vector<int> values(features.size());
    for (size_t i = 0; i < features.size(); i++) {
        values[i] = ValueOf(ini, features[i], enhanced);
        for (size_t j = 0; j < i && values[i]; j++) {
            if (values[j] && Exclusive(features[i], features[j])) {
                values[i] = 0;
            }
        }
    }
    return values;
}

bool FeatureChosen(const Ini &ini, const FeatureInfo &feature) {
    return !(feature.flags & kFeatureSession) && HasValue(ini, feature);
}

void SetFeature(Ini &ini, const std::vector<FeatureInfo> &features, size_t index, int value) {
    if (index >= features.size() || (features[index].flags & kFeatureSession)) {
        return;
    }
    MigrateFeatures(ini, features);
    if (Preset(ini) != "custom") {
        const std::vector<int> values = FeatureValues(ini, features);
        for (size_t i = 0; i < features.size(); i++) {
            if (!(features[i].flags & kFeatureSession) && !HasValue(ini, features[i])) {
                Write(ini, features[i], values[i]);
            }
        }
        ini.set(kSection, "preset", "custom");
    }
    Write(ini, features[index], value);
    for (size_t i = 0; value && i < features.size(); i++) {
        if (i == index || !Exclusive(features[i], features[index])) {
            continue;
        }
        if (ById(features[i]) ? StoredChoice(ini, features[i]) != 0 : !SettingOff(ini.get(kSection, features[i].name))) {
            Write(ini, features[i], 0);
        }
    }
}

bool MigrateFeatures(Ini &ini, const std::vector<FeatureInfo> &features) {
    bool changed = false;
    for (const FeatureInfo &feature : features) {
        if (!ById(feature) || (feature.flags & kFeatureSession)) {
            continue;
        }
        // The value: name_id from the number, or the number made to agree with name_id (an id it does not know left)
        const std::string id = ini.get(kSection, IdKey(feature));
        const std::string number = ini.get(kSection, feature.name);
        if ((!id.empty() || !number.empty()) && (id.empty() || SettingOff(id) || ChoiceOfId(feature, id) >= 0)) {
            const std::string &want = feature.ids[static_cast<size_t>(std::max(StoredChoice(ini, feature), 0))];
            const int old = NumberOfId(feature, want);
            changed |= Put(ini, kSection, IdKey(feature), want);
            changed |= Put(ini, kSection, feature.name, old >= 0 ? std::to_string(old) : "");
        }
        // The choices its menu offers, the same way
        const std::string ids = ini.get("menu", IdKey(feature));
        if ((feature.flags & kFeatureMenuChoices) && (!ids.empty() || !ini.get("menu", feature.name).empty()) && KnownIds(feature, ids)) {
            std::string byName, byNumber;
            MenuTexts(feature, MenuChoices(ini, feature), byName, byNumber);
            changed |= Put(ini, "menu", IdKey(feature), byName);
            changed |= Put(ini, "menu", feature.name, byNumber);
        }
    }
    return changed;
}

void SetPreset(Ini &ini, const std::vector<FeatureInfo> &features, const std::string &preset) {
    ini.set(kSection, "preset", preset);
    if (preset != "custom") {
        for (const FeatureInfo &feature : features) {
            if (!ini.get(kSection, feature.name).empty()) {
                ini.set(kSection, feature.name, ""); // (as the preset has it)
            }
            if (ById(feature) && !ini.get(kSection, IdKey(feature)).empty()) {
                ini.set(kSection, IdKey(feature), "");
            }
        }
    }
}

std::vector<bool> MenuChoices(const Ini &ini, const FeatureInfo &feature) {
    std::vector<bool> offered(feature.choices.size(), true);
    if (!(feature.flags & kFeatureMenuChoices)) {
        return offered;
    }
    // (by name, a feature with ids: name_id; otherwise, or none, by number: name, in the old order for one with ids)
    const std::string ids = ById(feature) ? ini.get("menu", IdKey(feature)) : "";
    const std::string list = ini.get("menu", feature.name);
    if (ids.empty() && list.empty()) {
        return offered;
    }
    std::fill(offered.begin(), offered.end(), false);
    if (!ids.empty()) {
        std::istringstream in(ids);
        for (std::string id; std::getline(in, id, ',');) {
            if (const int choice = ChoiceOfId(feature, Trimmed(id)); choice > 0) {
                offered[static_cast<size_t>(choice)] = true;
            }
        }
    } else {
        std::istringstream in(list);
        for (std::string number; std::getline(in, number, ',');) {
            char *end = nullptr;
            long choice = std::strtol(number.c_str(), &end, 10);
            if (end == number.c_str()) {
                continue;
            }
            if (ById(feature)) {
                choice = ChoiceOfId(feature, IdOfNumber(feature, choice));
            }
            if (choice > 0 && choice < static_cast<long>(offered.size())) {
                offered[static_cast<size_t>(choice)] = true;
            }
        }
    }
    if (!offered.empty()) {
        offered[0] = true;
    }
    return offered;
}

void SetMenuChoices(Ini &ini, const FeatureInfo &feature, const std::vector<bool> &offered) {
    std::string ids, numbers;
    MenuTexts(feature, offered, ids, numbers);
    if (ById(feature)) {
        Put(ini, "menu", IdKey(feature), ids);
    }
    ini.set("menu", feature.name, numbers);
}

} // namespace recomp
