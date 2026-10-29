#include <NF/Core/CommandLine.hpp>
#include <sstream>
#include <algorithm>

namespace nf::core {

CommandLineParser::CommandLineParser(std::string program_description)
    : m_description(std::move(program_description)) {}

void CommandLineParser::add_flag(std::string short_name, std::string long_name, std::string description) {
    OptionDef def;
    def.short_name = std::move(short_name);
    def.long_name = std::move(long_name);
    def.description = std::move(description);
    def.type = OptionType::Flag;
    m_defs.push_back(std::move(def));
}

void CommandLineParser::add_option(std::string short_name, std::string long_name, std::string description, std::string default_value) {
    OptionDef def;
    def.short_name = std::move(short_name);
    def.long_name = std::move(long_name);
    def.description = std::move(description);
    def.default_value = default_value;
    def.type = OptionType::Value;
    m_defs.push_back(std::move(def));
}

bool CommandLineParser::parse(int argc, const char* const* argv) {
    std::vector<std::string> args;
    if (argv && argc > 1) {
        args.reserve(static_cast<size_t>(argc - 1));
        for (int i = 1; i < argc; ++i) {
            if (argv[i]) {
                args.emplace_back(argv[i]);
            }
        }
    }
    return parse(args);
}

bool CommandLineParser::parse(const std::vector<std::string>& args) {
    m_parsed_values.clear();
    m_positional.clear();

    // Populate defaults for options
    for (const auto& def : m_defs) {
        if (def.type == OptionType::Value && !def.default_value.empty()) {
            if (!def.long_name.empty()) m_parsed_values[def.long_name] = def.default_value;
            if (!def.short_name.empty()) m_parsed_values[def.short_name] = def.default_value;
        }
    }

    size_t i = 0;
    while (i < args.size()) {
        const std::string& token = args[i];

        if (token.rfind("--", 0) == 0 && token.size() > 2) {
            // Long option: --name or --name=value
            const size_t eq_pos = token.find('=');
            std::string opt_name;
            std::string opt_val;

            if (eq_pos != std::string::npos) {
                opt_name = token.substr(2, eq_pos - 2);
                opt_val = token.substr(eq_pos + 1);
            } else {
                opt_name = token.substr(2);
            }

            const OptionDef* found_def = nullptr;
            for (const auto& def : m_defs) {
                if (def.long_name == opt_name) {
                    found_def = &def;
                    break;
                }
            }

            if (found_def && found_def->type == OptionType::Flag) {
                m_parsed_values[found_def->long_name] = "true";
                if (!found_def->short_name.empty()) m_parsed_values[found_def->short_name] = "true";
                i++;
            } else if (found_def && found_def->type == OptionType::Value) {
                if (eq_pos != std::string::npos) {
                    m_parsed_values[found_def->long_name] = opt_val;
                    if (!found_def->short_name.empty()) m_parsed_values[found_def->short_name] = opt_val;
                    i++;
                } else if (i + 1 < args.size()) {
                    m_parsed_values[found_def->long_name] = args[i + 1];
                    if (!found_def->short_name.empty()) m_parsed_values[found_def->short_name] = args[i + 1];
                    i += 2;
                } else {
                    return false; // missing value
                }
            } else {
                // Unknown flag treated as boolean flag
                m_parsed_values[opt_name] = (eq_pos != std::string::npos) ? opt_val : "true";
                i++;
            }
        } else if (token.rfind("-", 0) == 0 && token.size() > 1) {
            // Short option: -f or -f value
            const std::string opt_name = token.substr(1);
            const OptionDef* found_def = nullptr;
            for (const auto& def : m_defs) {
                if (def.short_name == opt_name) {
                    found_def = &def;
                    break;
                }
            }

            if (found_def && found_def->type == OptionType::Flag) {
                m_parsed_values[found_def->short_name] = "true";
                if (!found_def->long_name.empty()) m_parsed_values[found_def->long_name] = "true";
                i++;
            } else if (found_def && found_def->type == OptionType::Value) {
                if (i + 1 < args.size()) {
                    m_parsed_values[found_def->short_name] = args[i + 1];
                    if (!found_def->long_name.empty()) m_parsed_values[found_def->long_name] = args[i + 1];
                    i += 2;
                } else {
                    return false; // missing value
                }
            } else {
                m_parsed_values[opt_name] = "true";
                i++;
            }
        } else {
            // Positional argument
            m_positional.push_back(token);
            i++;
        }
    }

    return true;
}

bool CommandLineParser::has(const std::string& name) const {
    return m_parsed_values.find(name) != m_parsed_values.end();
}

std::string CommandLineParser::get_string(const std::string& name, const std::string& fallback) const {
    auto it = m_parsed_values.find(name);
    return (it != m_parsed_values.end()) ? it->second : fallback;
}

i64 CommandLineParser::get_int(const std::string& name, i64 fallback) const {
    auto it = m_parsed_values.find(name);
    if (it == m_parsed_values.end() || it->second.empty()) {
        return fallback;
    }
    try {
        return std::stoll(it->second);
    } catch (...) {
        return fallback;
    }
}

f64 CommandLineParser::get_float(const std::string& name, f64 fallback) const {
    auto it = m_parsed_values.find(name);
    if (it == m_parsed_values.end() || it->second.empty()) {
        return fallback;
    }
    try {
        return std::stod(it->second);
    } catch (...) {
        return fallback;
    }
}

std::string CommandLineParser::help_text() const {
    std::ostringstream ss;
    if (!m_description.empty()) {
        ss << m_description << "\n\n";
    }
    ss << "Options:\n";
    for (const auto& def : m_defs) {
        ss << "  ";
        if (!def.short_name.empty()) {
            ss << "-" << def.short_name << ", ";
        } else {
            ss << "    ";
        }
        if (!def.long_name.empty()) {
            ss << "--" << def.long_name;
        }
        if (def.type == OptionType::Value) {
            ss << " <value>";
        }
        ss << "\t\t" << def.description;
        if (!def.default_value.empty()) {
            ss << " (default: " << def.default_value << ")";
        }
        ss << "\n";
    }
    return ss.str();
}

} // namespace nf::core
