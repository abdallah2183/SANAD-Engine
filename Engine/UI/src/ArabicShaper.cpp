// NF/UI/ArabicShaper.cpp — contextual shaping + bidi reordering.
//
// Presentation-form table generated from the Unicode Character Database
// (Python unicodedata, authoritative names) — columns: base, isolated,
// final, initial, medial. 0 = no form. TATWEEL (U+0640) joins both sides
// with no substitution.

#include <NF/UI/ArabicShaper.hpp>

#include <cstdint>
#include <vector>

namespace nf::ui {

namespace {

struct LetterForms {
    char32_t base;
    char32_t isolated;
    char32_t final;
    char32_t initial;
    char32_t medial;
};

constexpr LetterForms kLetters[] = {
    {0x0621, 0xFE80, 0x0000, 0x0000, 0x0000}, // HAMZA
    {0x0622, 0xFE81, 0xFE82, 0x0000, 0x0000}, // ALEF WITH MADDA ABOVE
    {0x0623, 0xFE83, 0xFE84, 0x0000, 0x0000}, // ALEF WITH HAMZA ABOVE
    {0x0624, 0xFE85, 0xFE86, 0x0000, 0x0000}, // WAW WITH HAMZA ABOVE
    {0x0625, 0xFE87, 0xFE88, 0x0000, 0x0000}, // ALEF WITH HAMZA BELOW
    {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C}, // YEH WITH HAMZA ABOVE
    {0x0627, 0xFE8D, 0xFE8E, 0x0000, 0x0000}, // ALEF
    {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92}, // BEH
    {0x0629, 0xFE93, 0xFE94, 0x0000, 0x0000}, // TEH MARBUTA
    {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98}, // TEH
    {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}, // THEH
    {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0}, // JEEM
    {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}, // HAH
    {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}, // KHAH
    {0x062F, 0xFEA9, 0xFEAA, 0x0000, 0x0000}, // DAL
    {0x0630, 0xFEAB, 0xFEAC, 0x0000, 0x0000}, // THAL
    {0x0631, 0xFEAD, 0xFEAE, 0x0000, 0x0000}, // REH
    {0x0632, 0xFEAF, 0xFEB0, 0x0000, 0x0000}, // ZAIN
    {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}, // SEEN
    {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}, // SHEEN
    {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC}, // SAD
    {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}, // DAD
    {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}, // TAH
    {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8}, // ZAH
    {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC}, // AIN
    {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0}, // GHAIN
    {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4}, // FEH
    {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8}, // QAF
    {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC}, // KAF
    {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0}, // LAM
    {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}, // MEEM
    {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8}, // NOON
    {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC}, // HEH
    {0x0648, 0xFEED, 0xFEEE, 0x0000, 0x0000}, // WAW
    {0x0649, 0xFEEF, 0xFEF0, 0x0000, 0x0000}, // ALEF MAKSURA
    {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4}, // YEH
    {0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59}, // PEH
    {0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D}, // TCHEH
    {0x0698, 0xFB8A, 0xFB8B, 0x0000, 0x0000}, // JEH
    {0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95}, // GAF
    {0x06BE, 0xFBAA, 0xFBAB, 0xFBAC, 0xFBAD}, // HEH DOACHASHMEE
    {0x06C1, 0xFBA6, 0xFBA7, 0xFBA8, 0xFBA9}, // HEH GOAL
    {0x06CC, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF}, // FARSI YEH
    {0x06D2, 0xFBAE, 0xFBAF, 0x0000, 0x0000}, // YEH BARREE
};

constexpr char32_t kTatweel = 0x0640;

// Lam-alef ligatures: (alef variant, isolated, final).
constexpr struct LamAlef {
    char32_t alef;
    char32_t isolated;
    char32_t final;
} kLamAlef[] = {
    {0x0627, 0xFEFB, 0xFEFC}, // لا
    {0x0622, 0xFEF5, 0xFEF6}, // لآ
    {0x0623, 0xFEF7, 0xFEF8}, // لأ
    {0x0625, 0xFEF9, 0xFEFA}, // لإ
};

const LetterForms* find_letter(char32_t cp) {
    for (const auto& l : kLetters) {
        if (l.base == cp) return &l;
    }
    return nullptr;
}

// Joins toward the previous letter (visually: to the right).
bool joins_prev(char32_t cp) {
    if (cp == kTatweel) return true;
    const LetterForms* l = find_letter(cp);
    return l && (l->final != 0 || l->medial != 0);
}

// Joins toward the next letter (visually: to the left).
bool joins_next(char32_t cp) {
    if (cp == kTatweel) return true;
    const LetterForms* l = find_letter(cp);
    return l && (l->initial != 0 || l->medial != 0);
}

// --- UTF-8 helpers (private; NFCore has no UTF utilities yet) ---------------

bool decode_utf8(const std::string& s, std::vector<char32_t>& out) {
    out.clear();
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
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
            return false;
        }
        if (i + len > s.size()) return false;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        // Reject overlongs, surrogates, out-of-range.
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
            (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        out.push_back(cp);
        i += len;
    }
    return true;
}

void encode_utf8(char32_t cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Next/prev non-transparent (non-mark) codepoint in a run, or 0 at the edge.
char32_t run_prev_letter(const std::vector<char32_t>& run, size_t i) {
    while (i > 0) {
        --i;
        if (!is_arabic_mark(run[i])) return run[i];
    }
    return 0;
}
char32_t run_next_letter(const std::vector<char32_t>& run, size_t i) {
    for (size_t j = i + 1; j < run.size(); ++j) {
        if (!is_arabic_mark(run[j])) return run[j];
    }
    return 0;
}

} // namespace

bool is_arabic_letter(char32_t cp) {
    if (cp == kTatweel) return true;
    return find_letter(cp) != nullptr;
}

bool is_arabic_mark(char32_t cp) {
    // Harakat + friends: transparent for joining, kept with their base.
    return (cp >= 0x064B && cp <= 0x0652) || cp == 0x0670 || (cp >= 0x06D6 && cp <= 0x06DC) ||
           (cp >= 0x06DF && cp <= 0x06E4) || (cp >= 0x06E7 && cp <= 0x06E8) ||
           (cp >= 0x06EA && cp <= 0x06ED);
}

bool is_digit(char32_t cp) {
    return (cp >= U'0' && cp <= U'9') || (cp >= 0x0660 && cp <= 0x0669) ||
           (cp >= 0x06F0 && cp <= 0x06F9);
}

// Punctuation that can sit INSIDE a number and must therefore not split it.
//
// This is the whole of R1: a digit run used to be "digit+", so the '.' in "1.0"
// became its own cluster and the per-cluster reversal in shape_arabic() flipped
// the two digit fragments either side of it — "الإصدار 1.0" rendered as
// "الإصدار 0.1", "16:9" as "9:16", "-5" as "5-". Treating digits plus the
// punctuation between them as ONE LTR-preserving cluster fixes the class.
//
// Deliberately narrow: only characters that genuinely occur inside numbers. A
// blanket "any neutral joins a number" rule would swallow sentence punctuation
// and glue unrelated words together.
bool is_number_punct(char32_t cp) {
    switch (cp) {
        case U'.':
        case U',':
        case U':':
        case U'-':
        case U'/':
        case U'%':
        case U'+':
            return true;
        case 0x066B: // ARABIC DECIMAL SEPARATOR
        case 0x066A: // ARABIC PERCENT SIGN
            return true;
        default:
            return false;
    }
}

bool is_rtl_char(char32_t cp) {
    return is_arabic_letter(cp) || is_arabic_mark(cp) || is_digit(cp) ||
           // Arabic blocks that flow RTL but need no shaping.
           (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
           (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
           (cp >= 0xFE70 && cp <= 0xFEFF);
}

bool needs_shaping(const std::string& utf8) {
    for (unsigned char c : utf8) {
        if (c >= 0x80) return true; // any non-ASCII may need work
    }
    return false;
}

std::string shape_arabic(const std::string& logical_utf8) {
    if (!needs_shaping(logical_utf8)) return logical_utf8;
    std::vector<char32_t> cps;
    if (!decode_utf8(logical_utf8, cps)) return logical_utf8; // invalid: pass through

    // Split into directional runs. Neutrals (spaces, punctuation) join the
    // surrounding strong direction; at edges they go LTR.
    struct Run {
        size_t begin = 0, end = 0;
        bool rtl = false;
    };
    std::vector<Run> runs;
    {
        size_t i = 0;
        const size_t n = cps.size();
        while (i < n) {
            if (is_rtl_char(cps[i])) {
                size_t j = i;
                while (j < n) {
                    if (is_rtl_char(cps[j])) {
                        ++j;
                        continue;
                    }
                    // Neutral (space, punctuation): absorbed when the run is in
                    // RTL context on both sides. `j > i` is that context test —
                    // it says "this run already holds an RTL character", which
                    // keeps a neutral chain (" -") absorbed instead of stopping
                    // at the first one.
                    //
                    // The lookahead also treats a number punctuation that
                    // INTRODUCES a number as RTL-ish. Without that, the '-' of
                    // "السرعة -5" is not an RTL char, so the space and the sign
                    // split off into their own LTR run and the number ends up on
                    // the wrong side of the word entirely.
                    const bool next_is_rtl = (j + 1 < n) && is_rtl_char(cps[j + 1]);
                    const bool next_starts_number =
                        (j + 2 < n) && is_number_punct(cps[j + 1]) && is_digit(cps[j + 2]);
                    if (j > i && (next_is_rtl || next_starts_number)) {
                        ++j;
                        continue;
                    }
                    break;
                }
                runs.push_back({i, j, true});
                i = j;
            } else {
                size_t j = i;
                while (j < n && !is_rtl_char(cps[j])) ++j;
                runs.push_back({i, j, false});
                i = j;
            }
        }
    }

    std::string out;
    out.reserve(logical_utf8.size());
    for (const Run& run : runs) {
        if (!run.rtl) {
            for (size_t i = run.begin; i < run.end; ++i) encode_utf8(cps[i], out);
            continue;
        }
        // Shape the RTL run in logical order, then reverse (keeping digit
        // runs and mark+base pairs in place).
        std::vector<char32_t> shaped;
        shaped.reserve(run.end - run.begin);
        std::vector<char32_t> run_cps(cps.begin() + run.begin, cps.begin() + run.end);
        for (size_t i = 0; i < run_cps.size(); ++i) {
            const char32_t cp = run_cps[i];
            if (is_arabic_mark(cp)) {
                shaped.push_back(cp); // attaches to the previous base
                continue;
            }
            if (is_digit(cp)) {
                shaped.push_back(cp);
                continue;
            }
            const LetterForms* form = find_letter(cp);
            if (!form && cp != kTatweel) {
                shaped.push_back(cp); // punctuation etc: pass through
                continue;
            }
            if (cp == kTatweel) {
                shaped.push_back(cp);
                continue;
            }
            // Lam-alef ligature: lam + optional marks + alef variant.
            if (cp == 0x0644) {
                size_t k = i + 1;
                while (k < run_cps.size() && is_arabic_mark(run_cps[k])) ++k;
                if (k < run_cps.size()) {
                    const LamAlef* lig = nullptr;
                    for (const auto& cand : kLamAlef) {
                        if (cand.alef == run_cps[k]) {
                            lig = &cand;
                            break;
                        }
                    }
                    if (lig) {
                        const char32_t prev = run_prev_letter(run_cps, i);
                        const bool connect_prev = prev != 0 && joins_next(prev);
                        shaped.push_back(connect_prev ? lig->final : lig->isolated);
                        // Marks between lam and alef ride after the ligature.
                        for (size_t m = i + 1; m < k; ++m) shaped.push_back(run_cps[m]);
                        i = k; // skip the alef
                        continue;
                    }
                }
            }
            const char32_t prev = run_prev_letter(run_cps, i);
            const char32_t next = run_next_letter(run_cps, i);
            const bool connect_prev = prev != 0 && joins_next(prev);
            const bool connect_next = next != 0 && joins_prev(next);
            char32_t out_cp = form->isolated;
            if (connect_prev && connect_next) {
                // Medial position: medial form, else the final form (right-
                // joining-only letters like alef still connect backwards).
                out_cp = form->medial != 0 ? form->medial
                         : form->final != 0 ? form->final
                                            : form->isolated;
            } else if (connect_prev) {
                out_cp = form->final != 0 ? form->final : form->isolated;
            } else if (connect_next) {
                out_cp = form->initial != 0 ? form->initial : form->isolated;
            }
            shaped.push_back(out_cp != 0 ? out_cp : cp);
        }
        // Reverse the run by clusters so combining marks stay glued to their
        // base and digit sub-runs keep their order: [base marks*] | [number].
        //
        // A "number" is digits plus the punctuation BETWEEN them (is_number_punct
        // above). The scan starts at a digit, or at a number-punctuation that is
        // immediately followed by a digit — that second case is what keeps the
        // leading '-' of "-5" attached to its digits instead of becoming a
        // separate cluster that the reversal would move to the other side.
        struct Cluster {
            size_t begin = 0, end = 0;
        };
        std::vector<Cluster> clusters;
        for (size_t i = 0; i < shaped.size();) {
            const bool starts_number =
                is_digit(shaped[i]) ||
                (is_number_punct(shaped[i]) && i + 1 < shaped.size() && is_digit(shaped[i + 1]));
            if (starts_number) {
                size_t j = i;
                while (j < shaped.size() && (is_digit(shaped[j]) || is_number_punct(shaped[j]))) {
                    ++j;
                }
                // A number must END on a digit: trailing punctuation ("1." or the
                // ':' before a word) belongs to the surrounding text, so give it
                // back rather than dragging it into the reversed unit.
                while (j > i + 1 && !is_digit(shaped[j - 1])) {
                    --j;
                }
                clusters.push_back({i, j});
                i = j;
            } else if (is_arabic_mark(shaped[i])) {
                clusters.push_back({i, i + 1}); // stray mark: own cluster
                ++i;
            } else {
                size_t j = i + 1;
                while (j < shaped.size() && is_arabic_mark(shaped[j])) ++j;
                clusters.push_back({i, j});
                i = j;
            }
        }
        for (size_t c = clusters.size(); c > 0; --c) {
            const Cluster& cl = clusters[c - 1];
            for (size_t k = cl.begin; k < cl.end; ++k) encode_utf8(shaped[k], out);
        }
    }
    return out;
}

} // namespace nf::ui
