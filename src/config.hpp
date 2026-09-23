// Settings: config/turninplace.ini, read at load and re-read on the game thread when the file changes.
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later.
#pragma once

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>

namespace dwtip
{
    // Relative to the process working directory, Dawnwalker/Binaries/Win64. Not under scripts/: UE4SS makes a Lua
    // mod of any folder with a scripts subfolder and logs a missing main.lua.
    constexpr const char* SETTINGS_PATH = "ue4ss/Mods/DWTurnInPlace/config/turninplace.ini";

    struct Settings
    {
        bool enabled = true;
        double turn_angle = 50.0;   // degrees between camera and facing before a turn
        double settle_speed = 30.0; // camera degrees per second below which it counts as settled
        double settle_time = 0.3;   // seconds settled before a turn starts
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
        static constexpr const char* KEYS[] = {"enabled", "turn_angle", "settle_speed", "settle_time", "chain_turns", "toggle_key", "log_level"};
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
            case 6:
                if (lower(value) == "normal") s.verbose = false;
                else if (lower(value) == "verbose") s.verbose = true;
                else ok = false;
                break;
            }
            bad[at] = !ok;
        }

        // The game's own TurnInPlaceYawOffset is 45: at or below it the pushed mode only turns the head, and the push
        // is dropped as no_turn a second later.
        s.turn_angle = std::clamp(s.turn_angle, 46.0, 170.0);
        s.settle_speed = std::clamp(s.settle_speed, 1.0, 720.0);
        s.settle_time = std::clamp(s.settle_time, 0.0, 3.0);

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
} // namespace dwtip
