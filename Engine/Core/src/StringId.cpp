#include <NF/Core/StringId.hpp>
#include <cctype>
#include <algorithm>
#include <cstring>

namespace nf {

StringInterner& StringInterner::instance() {
    static StringInterner inst;
    return inst;
}

StringId StringInterner::intern(std::string_view str) {
    if (str.empty()) {
        return StringId();
    }
    uint64_t h = fnv1a_64(str);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_strings.emplace(h, std::string(str));
    return StringId(h);
}

std::string_view StringInterner::lookup(uint64_t hash) const {
    if (hash == 0) {
        return {};
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_strings.find(hash);
    if (it != m_strings.end()) {
        return it->second;
    }
    return {};
}

void StringInterner::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_strings.clear();
}

StringId::StringId(const char* str) {
    if (str && str[0] != '\0') {
        std::string_view sv(str);
        m_hash = fnv1a_64(sv);
        StringInterner::instance().intern(sv);
    }
}

StringId::StringId(std::string_view str) {
    if (!str.empty()) {
        m_hash = fnv1a_64(str);
        StringInterner::instance().intern(str);
    }
}

StringId::StringId(const std::string& str) {
    if (!str.empty()) {
        m_hash = fnv1a_64(str);
        StringInterner::instance().intern(str);
    }
}

std::string_view StringId::str() const {
    return StringInterner::instance().lookup(m_hash);
}

const char* StringId::c_str() const {
    std::string_view sv = str();
    return sv.empty() ? "" : sv.data();
}

namespace strings {

std::string trim(std::string_view str) {
    size_t first = 0;
    while (first < str.size() && std::isspace(static_cast<unsigned char>(str[first]))) {
        ++first;
    }
    if (first >= str.size()) {
        return {};
    }
    size_t last = str.size();
    while (last > first && std::isspace(static_cast<unsigned char>(str[last - 1]))) {
        --last;
    }
    return std::string(str.substr(first, last - first));
}

std::vector<std::string> split(std::string_view str, char delimiter) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start < str.size()) {
        size_t pos = str.find(delimiter, start);
        if (pos == std::string_view::npos) {
            result.emplace_back(str.substr(start));
            break;
        }
        result.emplace_back(str.substr(start, pos - start));
        start = pos + 1;
    }
    if (!str.empty() && str.back() == delimiter) {
        result.emplace_back("");
    }
    return result;
}

std::string join(const std::vector<std::string>& parts, std::string_view delimiter) {
    if (parts.empty()) {
        return {};
    }
    size_t total_len = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        total_len += parts[i].size();
        if (i + 1 < parts.size()) {
            total_len += delimiter.size();
        }
    }
    std::string result;
    result.reserve(total_len);
    for (size_t i = 0; i < parts.size(); ++i) {
        result += parts[i];
        if (i + 1 < parts.size()) {
            result += delimiter;
        }
    }
    return result;
}

std::string to_lower(std::string_view str) {
    std::string res;
    res.reserve(str.size());
    for (char c : str) {
        res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return res;
}

std::string to_upper(std::string_view str) {
    std::string res;
    res.reserve(str.size());
    for (char c : str) {
        res.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return res;
}

bool starts_with(std::string_view str, std::string_view prefix) {
    if (prefix.size() > str.size()) {
        return false;
    }
    return str.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view str, std::string_view suffix) {
    if (suffix.size() > str.size()) {
        return false;
    }
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string replace_all(std::string_view str, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return std::string(str);
    }
    std::string result;
    size_t start = 0;
    while (start < str.size()) {
        size_t pos = str.find(from, start);
        if (pos == std::string_view::npos) {
            result.append(str.substr(start));
            break;
        }
        result.append(str.substr(start, pos - start));
        result.append(to);
        start = pos + from.size();
    }
    return result;
}

} // namespace strings

} // namespace nf
