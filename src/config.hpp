// Settings: config/turninplace.ini, read at load and re-read on the game thread when the file changes.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dwtip
{
    // Relative to the process working directory, Dawnwalker/Binaries/Win64. Not under scripts/: UE4SS makes a Lua
    // mod of any folder with a scripts subfolder and logs a missing main.lua.
    constexpr const char* SETTINGS_PATH = "ue4ss/Mods/DWTurnInPlace/config/turninplace.ini";

    struct Settings
    {
        bool enabled = true;
        double turn_angle = 60.0;   // degrees between camera and facing before a turn
        double settle_speed = 60.0; // camera degrees per second below which it counts as settled
        double settle_time = 0.35;  // seconds settled before a turn starts
        double cancel_speed = 180.0; // smoothed camera degrees per second above which a held turn is cancelled
        bool chain_turns = true;
        std::string toggle_key;
        bool verbose = false;
    };

    struct Parsed
    {
        Settings settings;
        std::string defaulted; // "key (missing), key (invalid)", empty when every key was read
    };

    inline auto trim(std::string v) -> std::string
    {
        // ASCII whitespace only: std::isspace depends on the C locale, and UTF-8 bytes must survive.
        auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\v' && c != '\f'; };
        v.erase(v.begin(), std::find_if(v.begin(), v.end(), not_space));
        v.erase(std::find_if(v.rbegin(), v.rend(), not_space).base(), v.end());
        return v;
    }

    inline auto lower(std::string v) -> std::string
    {
        for (auto& c : v)
        {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return v;
    }

    inline auto parse_flag(const std::string& value, bool& out) -> bool
    {
        const auto v = lower(value);
        if (v == "true" || v == "1") out = true;
        else if (v == "false" || v == "0") out = false;
        else return false;
        return true;
    }

    inline auto parse_number(const std::string& value, double& out) -> bool
    {
        // std::stod accepts "nan" and "inf"; the clamps would pass them through.
        try
        {
            double v = std::stod(value);
            if (!std::isfinite(v)) return false;
            out = v;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    inline auto parse_settings(const std::string& content) -> Parsed
    {
        Parsed parsed;
        auto& s = parsed.settings;
        static constexpr const char* KEYS[] = {"enabled", "turn_angle", "settle_speed", "settle_time", "chain_turns", "toggle_key", "verbose_log", "cancel_speed"};
        constexpr int COUNT = static_cast<int>(std::size(KEYS));
        bool seen[COUNT]{};
        bool bad[COUNT]{};

        std::istringstream in(content);
        std::string line;
        while (std::getline(in, line))
        {
            if (auto cut = line.find_first_of(";#"); cut != std::string::npos) line.resize(cut);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const auto key = lower(trim(line.substr(0, eq)));
            const auto value = trim(line.substr(eq + 1));
            const auto at = std::find_if(std::begin(KEYS), std::end(KEYS), [&](const char* k) { return key == k; }) - std::begin(KEYS);
            if (at >= COUNT) continue; // unknown keys, such as crouch from 0.1.0, are ignored
            seen[at] = true;
            bool ok = true;
            switch (at)
            {
            case 0: ok = parse_flag(value, s.enabled); break;
            case 1: ok = parse_number(value, s.turn_angle); break;
            case 2: ok = parse_number(value, s.settle_speed); break;
            case 3: ok = parse_number(value, s.settle_time); break;
            case 4: ok = parse_flag(value, s.chain_turns); break;
            case 5: s.toggle_key = value; break; // blank is valid: unbound
            case 6: ok = parse_flag(value, s.verbose); break;
            case 7: ok = parse_number(value, s.cancel_speed); break;
            }
            bad[at] = !ok;
        }

        // The game's own TurnInPlaceYawOffset is 45: at or below it the pushed mode only turns the head, and the push
        // is dropped as no_turn a second later.
        s.turn_angle = std::clamp(s.turn_angle, 46.0, 170.0);
        // Matches the Mod Menu page (mod_settings.ini): a value outside its ConfigKey range fails the whole page open.
        s.settle_speed = std::clamp(s.settle_speed, 5.0, 180.0);
        s.settle_time = std::clamp(s.settle_time, 0.0, 2.0);
        s.cancel_speed = std::clamp(s.cancel_speed, 30.0, 360.0);

        for (int i = 0; i < COUNT; ++i)
        {
            if (seen[i] && !bad[i]) continue;
            if (!parsed.defaulted.empty()) parsed.defaulted += ", ";
            parsed.defaulted += KEYS[i];
            parsed.defaulted += seen[i] ? " (invalid)" : " (missing)";
        }
        return parsed;
    }

    inline auto read_file(const std::string& path) -> std::optional<std::string>
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // Temp file plus rename, so a reader (the Mod Menu included) never sees a half-written file.
    inline auto write_file(const std::string& path, const std::string& content) -> bool
    {
        auto tmp = path + ".dwtip.tmp";
        bool written = false;
        {
            std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
            if (file)
            {
                file << content;
                file.flush();
                written = static_cast<bool>(file);
            }
        }
        if (written && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DeleteFileA(tmp.c_str());
        return false;
    }

    // The shipped default line for each key, exactly as written in mod/config/turninplace.ini: value plus comment.
    // with_missing_keys appends whichever of these the file lacks; keeping this in step with that file is manual.
    inline auto shipped_line(const std::string& key) -> const char*
    {
        static const std::map<std::string, const char*> lines{
            {"enabled", "enabled = 1            ; master switch (1/0, true/false)"},
            {"turn_angle", "turn_angle = 60        ; degrees between camera and facing before a turn (clamp 46..170)"},
            {"settle_speed", "settle_speed = 60      ; camera turning slower than this, in degrees per second, counts as settled (clamp 5..180)"},
            {"settle_time", "settle_time = 0.35     ; seconds settled before a turn starts (clamp 0..2)"},
            {"chain_turns", "chain_turns = 1        ; keep turning while the camera pans slowly (1/0, true/false)"},
            {"toggle_key", "toggle_key =           ; F1-F12 or a letter/digit; blank = unbound"},
            {"cancel_speed", "cancel_speed = 180     ; a camera swinging faster than this, in degrees per second, stops a turn (clamp 30..360)"},
            {"verbose_log", "verbose_log = 0        ; 0: load, errors, toggle; 1: every push and pop with reason, offset, flags"},
        };
        auto found = lines.find(key);
        return found == lines.end() ? nullptr : found->second;
    }

    // Appends any of the 8 keys missing from the file, each as its shipped default line, and returns the added
    // key names in the order above. Called at startup only: a live poll must never call this, since the Mod
    // Menu's Apply briefly renames the file away and a poll landing in that gap must not read it as missing keys.
    inline auto with_missing_keys(const std::string& content) -> std::pair<std::string, std::vector<std::string>>
    {
        static constexpr const char* KEYS[] = {"enabled", "turn_angle", "settle_speed", "settle_time", "chain_turns", "toggle_key", "verbose_log", "cancel_speed"};
        bool seen[std::size(KEYS)]{};

        std::istringstream in(content);
        std::string line;
        while (std::getline(in, line))
        {
            if (auto cut = line.find_first_of(";#"); cut != std::string::npos) line.resize(cut);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const auto key = lower(trim(line.substr(0, eq)));
            for (size_t i = 0; i < std::size(KEYS); ++i)
            {
                if (key == KEYS[i]) seen[i] = true;
            }
        }

        std::vector<std::string> added;
        std::string out = content;
        bool needs_newline = !out.empty() && out.back() != '\n';
        for (size_t i = 0; i < std::size(KEYS); ++i)
        {
            if (seen[i]) continue;
            const char* text = shipped_line(KEYS[i]);
            if (!text) continue;
            if (needs_newline)
            {
                out += '\n';
                needs_newline = false;
            }
            out += text;
            out += '\n';
            added.emplace_back(KEYS[i]);
        }
        return {out, added};
    }
} // namespace dwtip
