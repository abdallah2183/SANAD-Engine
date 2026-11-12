// NF/UI/TextLayout.cpp — measured wrapping + tooltip sizing (see header).

#include <NF/UI/TextLayout.hpp>
#include <NF/UI/Widgets.hpp>

#include <algorithm>
#include <cmath>

namespace nf::ui {
namespace {

std::string shape_once(std::string_view logical) {
    return shape_arabic(std::string(logical));
}

float measure_or_fallback(const std::string& visual, float size, const TextMeasurer& m) {
    if (m) return m(visual, size);
    return fallback_text_width(visual, size);
}

} // namespace

float fallback_text_width(const std::string& shaped_text, float font_size) {
    if (font_size <= 0.0f) font_size = 22.0f;
    // Width derives from decoded codepoints, not bytes, so an Arabic letter
    // (2-3 bytes) and an ASCII letter (1 byte) are both ~one glyph wide.
    // A caller that cares about real advances injects ImFont measurement.
    float w = 0.0f;
    for (size_t i = 0; i < shaped_text.size();) {
        char32_t cp = 0;
        const size_t len = utf8_decode_one(shaped_text, i, cp);
        if (len == 0) {
            w += font_size * 0.55f; // invalid byte: one narrow cell
            ++i;
            continue;
        }
        if (cp == U' ') {
            w += font_size * 0.32f;
        } else if (cp >= 0x1100 && cp <= 0x115F) {
            w += font_size * 1.0f; // CJK wide
        } else {
            w += font_size * 0.58f;
        }
        i += len;
    }
    return w;
}

size_t utf8_decode_one(std::string_view utf8, size_t offset, char32_t& out_cp) {
    if (offset >= utf8.size()) return 0;
    const unsigned char c = static_cast<unsigned char>(utf8[offset]);
    char32_t cp = 0;
    size_t len = 0;
    if (c < 0x80) {
        cp = c;
        len = 1;
    } else if ((c & 0xE0) == 0xC0) {
        cp = c & 0x1F;
        len = 2;
    } else if ((c & 0xF0) == 0xE0) {
        cp = c & 0x0F;
        len = 3;
    } else if ((c & 0xF8) == 0xF0) {
        cp = c & 0x07;
        len = 4;
    } else {
        return 0;
    }
    if (offset + len > utf8.size()) return 0;
    for (size_t k = 1; k < len; ++k) {
        const unsigned char d = static_cast<unsigned char>(utf8[offset + k]);
        if ((d & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (d & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
        (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    out_cp = cp;
    return len;
}

size_t utf8_encode(char32_t cp, std::string& out) {
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    if (cp > 0x10FFFF) return 0;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
        return 1;
    }
    if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        return 2;
    }
    if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        return 3;
    }
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    return 4;
}

bool utf8_valid(std::string_view utf8) {
    size_t i = 0;
    while (i < utf8.size()) {
        char32_t cp = 0;
        const size_t len = utf8_decode_one(utf8, i, cp);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

size_t utf8_next_boundary(std::string_view utf8, size_t offset) {
    if (offset >= utf8.size()) return utf8.size();
    char32_t cp = 0;
    const size_t len = utf8_decode_one(utf8, offset, cp);
    if (len == 0) return offset + 1; // invalid byte: step one byte
    return offset + len;
}

size_t utf8_prev_boundary(std::string_view utf8, size_t offset) {
    if (offset == 0 || offset > utf8.size()) return 0;
    // Lead bytes are 0xxxxxxx / 110xxxxx / 1110xxxx / 11110xxx; walk back
    // over at most 3 continuation bytes.
    size_t start = offset - 1;
    size_t back = 0;
    while (start > 0 && back < 3) {
        const unsigned char c = static_cast<unsigned char>(utf8[start]);
        if ((c & 0xC0) == 0x80) {
            --start;
            ++back;
        } else {
            break;
        }
    }
    if (start == offset) return offset; // at start
    // Validate the candidate sequence starts DRIVING at `start`.
    char32_t cp = 0;
    const size_t len = utf8_decode_one(utf8, start, cp);
    if (len != 0 && start + len == offset) return start;
    return offset; // malformed: stay
}

size_t utf8_codepoint_count(std::string_view utf8) {
    size_t n = 0;
    size_t i = 0;
    while (i < utf8.size()) {
        char32_t cp = 0;
        const size_t len = utf8_decode_one(utf8, i, cp);
        i += (len == 0 ? 1 : len);
        ++n;
    }
    return n;
}

size_t utf8_snap_boundary(std::string_view utf8, size_t offset) {
    if (offset >= utf8.size()) return utf8.size();
    // Snap DOWN to the nearest codepoint boundary at or before offset, so a
    // caret never lands inside a multi-byte sequence.
    size_t i = 0, best = 0;
    while (i < utf8.size()) {
        if (i > offset) break;
        best = i;
        char32_t cp = 0;
        const size_t len = utf8_decode_one(utf8, i, cp);
        i += (len == 0 ? 1 : len);
    }
    return best;
}

float line_height_for(float font_size) {
    if (font_size <= 0.0f) font_size = 22.0f;
    return std::max(1.0f, font_size * 1.22f);
}

std::string clip_word_to_width(std::string_view logical_word, float box_w, float font_size,
                               const TextMeasurer& measure) {
    if (box_w <= 0.0f) return {};
    if (logical_word.empty()) return {};
    const std::string visual = shape_once(logical_word);
    const float w = measure_or_fallback(visual, font_size, measure);
    if (std::isfinite(w) && w <= box_w) return visual;
    // Binary search the longest codepoint prefix of the LOGICAL word whose
    // shaped form + ellipsis fits. Shaping a prefix differs from shaping the
    // full word at the cut (joining), but that is exactly why we clip the
    // logical form and re-shape: the ligature at the edge is never split.
    static const char* kEllipsis = "\xE2\x80\xA6"; // …
    const float ell_w = measure_or_fallback(kEllipsis, font_size, measure);
    // Collect logical codepoint boundaries.
    std::vector<size_t> bounds;
    bounds.push_back(0);
    for (size_t i = 0; i < logical_word.size();) {
        char32_t cp = 0;
        const size_t len = utf8_decode_one(logical_word, i, cp);
        size_t step = (len == 0 ? 1 : len);
        i += step;
        bounds.push_back(i);
    }
    // Longest prefix (excluding the empty prefix) that fits with ellipsis.
    size_t best = 0;
    for (size_t k = bounds.size(); k-- > 0;) {
        const size_t cut = bounds[k];
        if (cut == 0) continue;
        const std::string cand_visual = shape_once(logical_word.substr(0, cut));
        const float cw = measure_or_fallback(cand_visual, font_size, measure);
        if (std::isfinite(cw) && cw + ell_w <= box_w) {
            best = cut;
            break;
        }
    }
    if (best == 0) {
        // Even one codepoint + ellipsis overflows: emit the ellipsis alone.
        // It is the only over-width result possible when box_w < one glyph.
        return kEllipsis;
    }
    return shape_once(logical_word.substr(0, best)) + kEllipsis;
}

std::vector<std::string> wrap_text(std::string_view logical, float box_w, float font_size,
                                   const TextMeasurer& measure) {
    const float usable = std::max(1.0f, box_w);
    std::vector<std::string> out;
    for (const std::string& hard : split_hard_lines(logical)) {
        if (hard.empty()) {
            out.emplace_back("");
            continue;
        }
        const std::vector<std::string> words = split_words(hard);
        if (words.empty()) {
            out.emplace_back("");
            continue;
        }
        std::string cur_logical;
        for (const std::string& word : words) {
            const std::string candidate = cur_logical.empty() ? word : (cur_logical + " " + word);
            const std::string cand_visual = shape_once(candidate);
            const float cand_w = measure_or_fallback(cand_visual, font_size, measure);
            if (cur_logical.empty()) {
                if (std::isfinite(cand_w) && cand_w <= usable) {
                    cur_logical = candidate;
                } else {
                    // Single word wider than the box: clip, never overflow.
                    out.push_back(clip_word_to_width(word, usable, font_size, measure));
                }
            } else if (std::isfinite(cand_w) && cand_w <= usable) {
                cur_logical = candidate;
            } else {
                out.push_back(shape_once(cur_logical));
                // Start a new line with this word (clip if needed).
                const std::string word_visual = shape_once(word);
                const float ww = measure_or_fallback(word_visual, font_size, measure);
                if (std::isfinite(ww) && ww <= usable) {
                    cur_logical = word;
                } else {
                    out.push_back(clip_word_to_width(word, usable, font_size, measure));
                    cur_logical.clear();
                }
            }
        }
        if (!cur_logical.empty()) out.push_back(shape_once(cur_logical));
    }
    if (out.empty()) out.emplace_back("");
    return out;
}

float wrapped_max_width(const std::vector<std::string>& visual_lines, float font_size,
                        const TextMeasurer& measure) {
    float m = 0.0f;
    for (const auto& l : visual_lines) {
        if (l.empty()) continue;
        const float w = measure_or_fallback(l, font_size, measure);
        if (std::isfinite(w)) m = std::max(m, w);
    }
    return m;
}

TooltipLayout layout_tooltip(std::string_view logical, float viewport_w, float viewport_h,
                             float pointer_x_px, float pointer_y_px, float font_size,
                             const TextMeasurer& measure) {
    constexpr float kPadX = 8.0f;
    constexpr float kPadY = 6.0f;
    constexpr float kScreenInset = 4.0f;
    constexpr float kMaxWidth = 360.0f;
    constexpr float kMinWidth = 120.0f;
    const float vw = std::max(1.0f, viewport_w);
    const float vh = std::max(1.0f, viewport_h);
    float max_w = std::min(kMaxWidth, std::max(kMinWidth, vw * 0.5f));
    max_w = std::min(max_w, std::max(kMinWidth, vw - 2.0f * kScreenInset));
    const float inner_w = std::max(1.0f, max_w - 2.0f * kPadX);
    std::vector<std::string> lines = wrap_text(logical, inner_w, font_size, measure);
    const float line_h = line_height_for(font_size);
    const float text_w = wrapped_max_width(lines, font_size, measure);
    // Auto-size: shrink to the widest line (plus padding), never exceed max.
    float box_w = std::min(max_w, std::max(kMinWidth, text_w + 2.0f * kPadX));
    if (lines.empty()) box_w = std::min(max_w, kMinWidth + 2.0f * kPadX);
    box_w = std::min(box_w, vw - 2.0f * kScreenInset);
    box_w = std::max(box_w, 1.0f);
    float box_h = static_cast<float>(lines.size()) * line_h + 2.0f * kPadY;
    // Height clamp: more lines than fit -> clip the line list (still in-bounds).
    const float max_h = std::max(line_h, vh - 2.0f * kScreenInset);
    if (box_h > max_h) {
        box_h = max_h;
        const size_t fit = static_cast<size_t>(std::max(1.0f, (max_h - 2.0f * kPadY) / line_h));
        if (lines.size() > fit) lines.resize(fit);
        box_h = static_cast<float>(lines.size()) * line_h + 2.0f * kPadY;
        box_h = std::min(box_h, max_h);
    }
    float x = pointer_x_px + 14.0f;
    float y = pointer_y_px + 14.0f;
    x = std::clamp(x, kScreenInset, std::max(kScreenInset, vw - box_w - kScreenInset));
    y = std::clamp(y, kScreenInset, std::max(kScreenInset, vh - box_h - kScreenInset));
    TooltipLayout out;
    out.x = x;
    out.y = y;
    out.w = box_w;
    out.h = box_h;
    out.lines = std::move(lines);
    return out;
}

} // namespace nf::ui
