#pragma once

#include <NF/Core/Types.hpp>
#include <string>
#include <vector>
#include <unordered_map>

namespace nf::core {

enum class OptionType {
    Flag,
    Value
};

struct OptionDef {
    std::string short_name;
    std::string long_name;
    std::string description;
    std::string default_value;
    OptionType type = OptionType::Flag;
};

/// Robust command line option and argument parser
class CommandLineParser {
public:
    explicit CommandLineParser(std::string program_description = "");

    void add_flag(std::string short_name, std::string long_name, std::string description);
    void add_option(std::string short_name, std::string long_name, std::string description, std::string default_value = "");

    /// Parse standard main(argc, argv) arguments
    bool parse(int argc, const char* const* argv);

    /// Parse tokenized argument strings
    bool parse(const std::vector<std::string>& args);

    /// Check if a flag or option was specified
    [[nodiscard]] bool has(const std::string& name) const;

    /// Get value of an option as string, integer, or float
    [[nodiscard]] std::string get_string(const std::string& name, const std::string& fallback = "") const;
    [[nodiscard]] i64 get_int(const std::string& name, i64 fallback = 0) const;
    [[nodiscard]] f64 get_float(const std::string& name, f64 fallback = 0.0) const;

    /// Positional (non-flag) arguments
    [[nodiscard]] const std::vector<std::string>& positional_args() const noexcept { return m_positional; }

    /// Format standard help usage string
    [[nodiscard]] std::string help_text() const;

private:
    std::string m_description;
    std::vector<OptionDef> m_defs;
    std::unordered_map<std::string, std::string> m_parsed_values;
    std::vector<std::string> m_positional;
};

} // namespace nf::core
