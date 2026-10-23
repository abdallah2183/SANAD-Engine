#include <NF/Runtime/PlayerSettings.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <vector>

namespace nf::runtime {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

bool equals_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

/// Parses a float and refuses anything that is not a finite number: a NaN
/// written by hand would otherwise scale the camera's sensitivity to nothing
/// and the player would report "the mouse stopped working".
bool parse_float(std::string_view text, float& out) {
    const std::string tmp(text);
    const char* first = tmp.data();
    const char* last = tmp.data() + tmp.size();
    float value = 0.0f;
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        return false;
    }
    if (!std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool in_range(float v, float lo, float hi) {
    return v >= lo && v <= hi;
}

} // namespace

bool parse_player_settings(std::string_view text, PlayerSettings& out, std::string& out_error) {
    PlayerSettings parsed; // applied only if the whole file parses
    size_t line_number = 0;
    size_t start = 0;

    while (start <= text.size()) {
        const size_t newline = text.find('\n', start);
        std::string_view line = text.substr(
            start, newline == std::string_view::npos ? std::string_view::npos : newline - start);
        start = (newline == std::string_view::npos) ? text.size() + 1 : newline + 1;
        ++line_number;

        // Comments run to the end of the line, and a comment on its own line
        // is the common case: the format is meant to be hand-edited.
        const size_t hash = line.find('#');
        if (hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            out_error = "line " + std::to_string(line_number) + ": expected 'key: value'";
            return false;
        }
        const std::string_view key = trim(line.substr(0, colon));
        const std::string_view value = trim(line.substr(colon + 1));
        if (value.empty()) {
            out_error = "line " + std::to_string(line_number) + ": '" + std::string(key) +
                        "' has no value";
            return false;
        }

        if (equals_ci(key, "version")) {
            // Reserved. A future format bump can refuse a file it cannot read
            // instead of silently misreading it; today every version is 1.
            float v = 0.0f;
            if (!parse_float(value, v) || v != 1.0f) {
                out_error = "line " + std::to_string(line_number) +
                            ": unsupported settings version '" + std::string(value) + "'";
                return false;
            }
            continue;
        }

        auto read_scale = [&](float& target, float lo, float hi) {
            float v = 0.0f;
            if (!parse_float(value, v) || !in_range(v, lo, hi)) {
                out_error = "line " + std::to_string(line_number) + ": '" + std::string(key) +
                            "' must be a number in [" + std::to_string(lo) + ", " +
                            std::to_string(hi) + "]";
                return false;
            }
            target = v;
            return true;
        };

        if (equals_ci(key, "look_scale")) {
            if (!read_scale(parsed.look_scale, 0.05f, 10.0f)) return false;
        } else if (equals_ci(key, "stick_scale")) {
            if (!read_scale(parsed.stick_scale, 0.05f, 10.0f)) return false;
        } else if (equals_ci(key, "ads_scale")) {
            if (!read_scale(parsed.ads_scale, 0.05f, 10.0f)) return false;
        } else if (equals_ci(key, "shake_scale")) {
            // 0 is legitimate here, unlike the sensitivity scales: a player
            // who cannot tolerate shake should be able to switch it off.
            if (!read_scale(parsed.shake_scale, 0.0f, 4.0f)) return false;
        } else if (equals_ci(key, "fov")) {
            float v = 0.0f;
            if (!parse_float(value, v) || (v != 0.0f && !in_range(v, 20.0f, 140.0f))) {
                out_error = "line " + std::to_string(line_number) +
                            ": 'fov' must be 0 (the scene's) or within [20, 140]";
                return false;
            }
            parsed.fov_y = v;
        } else if (key.size() > 7 && equals_ci(key.substr(0, 7), "volume.")) {
            // `volume.<bus>` — the same five names the mixer uses, so a
            // settings file and an audio log cannot disagree about what
            // "master" is called.
            audio::BusId bus{};
            if (!audio::bus_id_from_name(key.substr(7), bus)) {
                out_error = "line " + std::to_string(line_number) + ": unknown audio bus '" +
                            std::string(key.substr(7)) + "'";
                return false;
            }
            float v = 0.0f;
            if (!parse_float(value, v) || !in_range(v, 0.0f, 1.0f)) {
                out_error = "line " + std::to_string(line_number) + ": '" + std::string(key) +
                            "' must be a number in [0, 1]";
                return false;
            }
            parsed.volumes.set_volume(bus, v);
        } else if (key.size() > 15 && equals_ci(key.substr(0, 15), "audio.dynamics.")) {
            // Player-owned master dynamics. Ranges are deliberately wider than
            // the UI's sliders but still bounded: a threshold of -100000 dB or
            // a 10-minute attack is a hand-edit accident, not a preference.
            const std::string_view field = key.substr(15);
            auto read_dyn = [&](float& target, float lo, float hi) {
                float v = 0.0f;
                if (!parse_float(value, v) || !in_range(v, lo, hi)) {
                    out_error = "line " + std::to_string(line_number) + ": '" +
                                std::string(key) + "' must be a number in [" +
                                std::to_string(lo) + ", " + std::to_string(hi) + "]";
                    return false;
                }
                target = v;
                return true;
            };
            auto read_switch = [&](bool& target) {
                if (equals_ci(value, "on") || equals_ci(value, "true") ||
                    equals_ci(value, "1")) {
                    target = true;
                    return true;
                }
                if (equals_ci(value, "off") || equals_ci(value, "false") ||
                    equals_ci(value, "0")) {
                    target = false;
                    return true;
                }
                out_error = "line " + std::to_string(line_number) + ": '" +
                            std::string(key) + "' must be on or off";
                return false;
            };

            if (equals_ci(field, "compressor")) {
                if (!read_switch(parsed.dynamics.compressor.enabled)) return false;
            } else if (equals_ci(field, "threshold_db")) {
                if (!read_dyn(parsed.dynamics.compressor.threshold_db, -96.0f, 0.0f)) {
                    return false;
                }
            } else if (equals_ci(field, "ratio")) {
                if (!read_dyn(parsed.dynamics.compressor.ratio, 1.0f, 20.0f)) return false;
            } else if (equals_ci(field, "attack_ms")) {
                if (!read_dyn(parsed.dynamics.compressor.attack_ms, 0.0f, 500.0f)) {
                    return false;
                }
            } else if (equals_ci(field, "release_ms")) {
                if (!read_dyn(parsed.dynamics.compressor.release_ms, 0.0f, 5000.0f)) {
                    return false;
                }
            } else if (equals_ci(field, "knee_db")) {
                if (!read_dyn(parsed.dynamics.compressor.knee_db, 0.0f, 24.0f)) return false;
            } else if (equals_ci(field, "makeup_db")) {
                if (!read_dyn(parsed.dynamics.compressor.makeup_db, -24.0f, 24.0f)) {
                    return false;
                }
            } else if (equals_ci(field, "limiter")) {
                if (!read_switch(parsed.dynamics.limiter.enabled)) return false;
            } else if (equals_ci(field, "ceiling_db")) {
                if (!read_dyn(parsed.dynamics.limiter.ceiling_db, -24.0f, 0.0f)) {
                    return false;
                }
            } else if (equals_ci(field, "limiter_release_ms")) {
                if (!read_dyn(parsed.dynamics.limiter.release_ms, 0.0f, 5000.0f)) {
                    return false;
                }
            } else if (equals_ci(field, "lookahead_ms")) {
                if (!read_dyn(parsed.dynamics.limiter.lookahead_ms, 0.0f, 50.0f)) {
                    return false;
                }
            } else {
                // A misspelled control inside a namespace we own is refused by
                // name. Silently ignoring it is exactly how a slider ends up
                // doing nothing with no diagnostic anywhere.
                out_error = "line " + std::to_string(line_number) +
                            ": unknown audio dynamics key '" + std::string(field) + "'";
                return false;
            }
        } else if (equals_ci(key, "invert_y")) {
            if (equals_ci(value, "on") || equals_ci(value, "true") || equals_ci(value, "1")) {
                parsed.invert_y = InvertY::On;
            } else if (equals_ci(value, "off") || equals_ci(value, "false") ||
                       equals_ci(value, "0")) {
                parsed.invert_y = InvertY::Off;
            } else if (equals_ci(value, "auto") || equals_ci(value, "scene")) {
                parsed.invert_y = InvertY::Scene;
            } else {
                out_error = "line " + std::to_string(line_number) +
                            ": 'invert_y' must be on, off or auto";
                return false;
            }
        }
        // Unknown keys are ignored on purpose: a settings file written by a
        // newer build must still load in an older one, minus what it does not
        // understand.
    }

    out = parsed;
    return true;
}

std::string serialize_player_settings(const PlayerSettings& settings) {
    std::ostringstream out;
    out << "# SANAD player settings — written by the engine, safe to hand-edit.\n";
    out << "# Every value here OVERRIDES the scene; deleting this file restores\n";
    out << "# exactly what the level's author chose.\n";
    out << "version: 1\n";
    out << "look_scale: " << settings.look_scale << "\n";
    out << "stick_scale: " << settings.stick_scale << "\n";
    switch (settings.invert_y) {
    case InvertY::On: out << "invert_y: on\n"; break;
    case InvertY::Off: out << "invert_y: off\n"; break;
    case InvertY::Scene:
    default: out << "invert_y: auto\n"; break;
    }
    out << "fov: " << settings.fov_y << "\n";
    out << "ads_scale: " << settings.ads_scale << "\n";
    out << "shake_scale: " << settings.shake_scale << "\n";
    for (u32 i = 0; i < audio::kBusCount; ++i) {
        const auto bus = static_cast<audio::BusId>(i);
        out << "volume." << audio::bus_name(bus) << ": " << settings.volumes.volume(bus) << "\n";
    }

    // 9 significant digits: a value written by the editor re-reads as the same
    // f32, which matters because a dynamics threshold is compared against a
    // level and a one-ulp drift should not show up as a changed file.
    out << std::setprecision(9);
    const auto on_off = [](bool enabled) { return enabled ? "on" : "off"; };
    out << "audio.dynamics.compressor: " << on_off(settings.dynamics.compressor.enabled)
        << "\n";
    out << "audio.dynamics.threshold_db: " << settings.dynamics.compressor.threshold_db << "\n";
    out << "audio.dynamics.ratio: " << settings.dynamics.compressor.ratio << "\n";
    out << "audio.dynamics.attack_ms: " << settings.dynamics.compressor.attack_ms << "\n";
    out << "audio.dynamics.release_ms: " << settings.dynamics.compressor.release_ms << "\n";
    out << "audio.dynamics.knee_db: " << settings.dynamics.compressor.knee_db << "\n";
    out << "audio.dynamics.makeup_db: " << settings.dynamics.compressor.makeup_db << "\n";
    out << "audio.dynamics.limiter: " << on_off(settings.dynamics.limiter.enabled) << "\n";
    out << "audio.dynamics.ceiling_db: " << settings.dynamics.limiter.ceiling_db << "\n";
    out << "audio.dynamics.limiter_release_ms: " << settings.dynamics.limiter.release_ms
        << "\n";
    out << "audio.dynamics.lookahead_ms: " << settings.dynamics.limiter.lookahead_ms << "\n";
    return out.str();
}


// --- profiles ----------------------------------------------------------------

bool valid_profile_name(std::string_view name) {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    if (name.front() == ' ' || name.back() == ' ') {
        return false;
    }
    for (const char c : name) {
        const auto uc = static_cast<unsigned char>(c);
        const bool ok = std::isalnum(uc) != 0 || c == ' ' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string profile_path(std::string_view name) {
    if (!valid_profile_name(name)) {
        return {};
    }
    return "content://Settings/players/" + std::string(name) + ".nfsettings";
}

std::vector<std::string> parse_profile_index(std::string_view text) {
    std::vector<std::string> names;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        std::string_view line =
            text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        start = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
        if (const size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        const std::string name(line);
        // A duplicate in a hand-edited index must not produce two identical
        // entries in the picker: the second would look like a broken button.
        if (!valid_profile_name(name)) {
            continue;
        }
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
    }
    return names;
}

std::string serialize_profile_index(const std::vector<std::string>& names) {
    std::string out = "# SANAD player profiles — one name per line, edited by the editor.\n";
    for (const auto& name : names) {
        if (valid_profile_name(name)) {
            out += name;
            out += '\n';
        }
    }
    return out;
}

} // namespace nf::runtime
