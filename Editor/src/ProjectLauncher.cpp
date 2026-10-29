// ProjectLauncher.cpp — the pre-editor Win32 project launcher (UX2 item E2).
//
// See ProjectLauncher.hpp for WHY this is a separate launcher and not an ImGui
// window: main.cpp applies the project's VFS mounts before it constructs the
// Runtime, and ImGui does not come up until after the Runtime, so a picker that
// runs where its answer is needed cannot be the editor's own UI.
//
// This translation unit is the Win32 shell only. Every decision it makes —
// recent-list ordering and de-duplication, name validation, the Documents
// default, the .nfproj path — lives in the header as a pure inline function so
// EditorTests can pin it without a window. If you find yourself adding policy
// here, put it in the header instead.
//
// Unicode: the UI is Arabic-first, so paths can be Arabic. Everything crosses
// the Win32 boundary as UTF-16 and is converted at the edges; the engine-side
// API stays UTF-8 std::string.
//
// Visual: a hand-drawn dark theme (forge palette) rather than the classic
// gray Win32 look — owner-draw buttons, an owner-draw recent list with a
// name/path row, Segoe UI, and a small brand header. Behaviour is unchanged:
// double-click opens, Create scaffolds, Quit exits clean.

#include <NF/Editor/ProjectLauncher.hpp>

#include <NF/Project/ProjectScaffold.hpp>

#include <NF/Core/Logger.hpp>
#include <NF/UI/ArabicShaper.hpp>
#include <NF/UI/Localization.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// shobjidl.h pulls in the modern IFileDialog; the legacy OPENFILENAMEA path in
// Toolbar.cpp cannot pick FOLDERS, which is exactly what New Project and E4 need.
#include <shobjidl.h>
// shlobj.h declares SHGetKnownFolderPath and knownfolders.h defines
// FOLDERID_Documents — neither comes with shobjidl.h.
#include <shlobj.h>
// SetWindowSubclass for hover tracking on owner-draw buttons.
#include <commctrl.h>
// ShellExecuteW for opening folders/URLs from Learn/Help cards.
#include <shellapi.h>
// GDI+ decodes the PNG thumbnails (Docs/images) to HBITMAPs. stb_image would
// need include-path plumbing into this target for one call site; GDI+ is
// already on every Windows machine and keeps the shell GPU-free (it runs
// before the device exists, so RHI textures are not an option here).
#include <gdiplus.h>
#endif

#include <cstdio>
#include <fstream>

namespace nf::editor {

namespace {

#ifdef _WIN32

// --- theme ------------------------------------------------------------------
// One dark palette for the whole launcher. Kept local: nothing outside this
// TU needs to know the colors, and the pure header stays UI-free.

namespace theme {

constexpr COLORREF bg = RGB(16, 16, 20);
constexpr COLORREF surface = RGB(26, 26, 32);
constexpr COLORREF surface_hi = RGB(34, 34, 42);
constexpr COLORREF list_bg = RGB(20, 20, 26);
constexpr COLORREF border = RGB(55, 55, 66);
constexpr COLORREF text = RGB(236, 236, 242);
constexpr COLORREF text_dim = RGB(145, 145, 160);
// One accent for the whole product: clean blue (#4C8DFF), the same value the
// editor theme defaults to. The old forge orange was a second identity that
// only the shell used, so launcher and editor disagreed about "primary".
constexpr COLORREF accent = RGB(76, 141, 255);
constexpr COLORREF accent_hi = RGB(110, 164, 255);
constexpr COLORREF accent_lo = RGB(59, 116, 219);
constexpr COLORREF on_accent = RGB(255, 255, 255);
constexpr COLORREF sel_bg = RGB(44, 44, 54);
constexpr COLORREF sel_bar = accent;
constexpr COLORREF error = RGB(255, 110, 110);
constexpr COLORREF ok = RGB(120, 210, 150);

} // namespace theme

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                        nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), n,
                          nullptr, nullptr);
    return out;
}

/// Reads an environment variable. `GetEnvironmentVariableA` rather than
/// `std::getenv`, which MSVC flags as unsafe (C4996) and this target builds with
/// /WX — a deprecation warning is a build break here, not a note.
std::string env_var(const char* name) {
    char buf[MAX_PATH * 4] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        return {};
    }
    return std::string(buf, n);
}

/// Directory of the running editor. Used to resolve data that ships beside the
/// executable (Templates/, Docs/, Resources/) in a portable package, where the
/// compile-time absolute paths of the developer's build tree do not exist.
std::filesystem::path launcher_module_dir() {
    wchar_t mod[MAX_PATH * 2] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, mod, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) {
        return {};
    }
    return std::filesystem::path(mod).parent_path();
}

// --- Arabic shell: RTL switch + translated/shaped text -----------------------
// ONE consistent RTL approach for the whole shell: manual coordinate mirroring
// (NOT WS_EX_LAYOUTRTL). Paint and hit-testing already share layout helpers,
// so mirroring inside those helpers keeps clicks and rendering identical by
// construction — a window-level layout flip would mirror paint but leave the
// hand-rolled hit rects behind. Documented here so the choice stays single.
bool g_shell_rtl = false; // true while the Arabic UI is active

// Translated logical UTF-8 for a shell key (goes to NATIVE controls, which
// shape natively — never pre-shape those or editing breaks).
inline std::string shell_tr(const char* key) {
    return nf::ui::tr(key);
}

// --- GDI visual-order shaping + bidi for ExtTextOutW ------------------------
//
// Why this exists in ProjectLauncher.cpp instead of calling shape_arabic()
// raw:
//   1. Windows gdi32!ExtTextOutW inspects the UTF-16 buffer for complex-script
//      codepoints (U+0590..U+08FF, bidi controls). Pure Presentation Forms-B
//      (U+FE70..U+FEFF) take GDI's plain LTR glyph path and render in the
//      exact visual order we computed. ANY remaining base-Arabic codepoint —
//      combining harakat/shadda/tanween (U+064B..U+065F) or Arabic punctuation
//      such as '۔' (U+06D4) at the end of subtitles — routes the whole string
//      through LpkExtTextOut (Uniscribe), which sees Bidi Class AL on
//      U+FE70..U+FEFF and reverses our already-visual string a SECOND time!
//   2. shape_arabic() assumes an LTR base paragraph when multiple runs exist
//      (pinned by UITests/EditorTests), which swaps clauses across ' — ', ': ',
//      parentheses, and embedded Latin words in RTL launcher sentences.
//   3. Multi-line status strings ('\n') must shape line-by-line so lines never
//      reverse across a newline boundary.
namespace gdi_bidi {

inline bool decode_u8(const std::string& s, std::vector<char32_t>& out) {
    out.clear();
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0;
        std::size_t len = 0;
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
        if (i + len > s.size()) {
            return false;
        }
        for (std::size_t k = 1; k < len; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (d & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
            (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        out.push_back(cp);
        i += len;
    }
    return true;
}

inline std::string encode_u8(const std::vector<char32_t>& cps, std::size_t b, std::size_t e) {
    std::string out;
    out.reserve((e - b) * 3);
    for (std::size_t i = b; i < e; ++i) {
        const char32_t cp = cps[i];
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
    return out;
}

inline void append_u16(std::wstring& out, char32_t cp) {
    if (cp <= 0xFFFF) {
        out.push_back(static_cast<wchar_t>(cp));
    } else if (cp <= 0x10FFFF) {
        const char32_t v = cp - 0x10000;
        out.push_back(static_cast<wchar_t>(0xD800 | (v >> 10)));
        out.push_back(static_cast<wchar_t>(0xDC00 | (v & 0x3FF)));
    }
}

inline bool is_combining_or_bidi_ctrl(char32_t cp) {
    return nf::ui::is_arabic_mark(cp) || (cp >= 0x0610 && cp <= 0x061A) ||
           (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670 || (cp >= 0x06D6 && cp <= 0x06ED) ||
           cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2066 && cp <= 0x2069);
}

inline bool is_strong_rtl(char32_t cp) {
    if (nf::ui::is_digit(cp) || is_combining_or_bidi_ctrl(cp)) {
        return false;
    }
    return (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
           (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
           (cp >= 0xFE70 && cp <= 0xFEFF);
}

inline bool is_strong_ltr(char32_t cp) {
    return (cp >= U'A' && cp <= U'Z') || (cp >= U'a' && cp <= U'z') ||
           (cp >= 0x00C0 && cp <= 0x024F && cp != 0x00D7 && cp != 0x00F7);
}

inline char32_t mirror_bracket(char32_t cp) {
    switch (cp) {
        case U'(': return U')';
        case U')': return U'(';
        case U'[': return U']';
        case U']': return U'[';
        case U'{': return U'}';
        case U'}': return U'{';
        case 0x00AB: return 0x00BB; // « -> »
        case 0x00BB: return 0x00AB; // » -> «
        case 0x2039: return 0x203A; // ‹ -> ›
        case 0x203A: return 0x2039; // › -> ‹
        default: return cp;
    }
}

// Emits a shaped codepoint for GDI while guaranteeing zero U+0590..U+08FF
// codepoints remain in the output buffer (preventing LpkExtTextOut from
// double-reversing Presentation Forms-B).
inline void append_gdi_safe(std::wstring& out, char32_t cp) {
    if (is_combining_or_bidi_ctrl(cp) || cp == 0x0640) {
        return; // marks/tatweel already participated in contextual joining
    }
    if (cp >= 0x0660 && cp <= 0x0669) {
        out.push_back(static_cast<wchar_t>(L'0' + (cp - 0x0660)));
        return;
    }
    if (cp >= 0x06F0 && cp <= 0x06F9) {
        out.push_back(static_cast<wchar_t>(L'0' + (cp - 0x06F0)));
        return;
    }
    switch (cp) {
        case 0x06D4: // ۔ Arabic full stop
        case 0x066B: // ٫ Arabic decimal separator
            out.push_back(L'.');
            return;
        case 0x060C: // ، Arabic comma
        case 0x066C: // ٬ Arabic thousands separator
            out.push_back(L',');
            return;
        case 0x061B: // ؛ Arabic semicolon
            out.push_back(L';');
            return;
        case 0x061F: // ؟ Arabic question mark
            out.push_back(L'?');
            return;
        case 0x066A: // ٪ Arabic percent sign
            out.push_back(L'%');
            return;
        default:
            break;
    }
    if (cp >= 0x0590 && cp <= 0x08FF) {
        return; // never let an unmapped complex-script codepoint flip the line
    }
    append_u16(out, cp);
}

inline std::wstring sanitize_shaped_utf8(const std::string& shaped_utf8) {
    std::vector<char32_t> cps;
    if (!decode_u8(shaped_utf8, cps)) {
        return widen(shaped_utf8);
    }
    std::wstring out;
    out.reserve(cps.size());
    for (char32_t cp : cps) {
        append_gdi_safe(out, cp);
    }
    return out;
}

inline bool is_num_bridge(char32_t cp) {
    return cp == U'.' || cp == U',' || cp == U':' || cp == U'-' || cp == U'/' || cp == U'%';
}

inline bool is_phrase_bridge(char32_t cp) {
    return cp == U' ' || cp == U'.' || cp == U',' || cp == U':' || cp == U'-' || cp == U'_' ||
           cp == U'/' || cp == U'\\' || cp == U'+' || cp == U'#' || cp == 0x00B7;
}

inline std::wstring shape_line_for_gdi(const std::string& line) {
    if (!nf::ui::needs_shaping(line)) {
        return widen(line);
    }
    std::vector<char32_t> cps;
    if (!decode_u8(line, cps) || cps.empty()) {
        return widen(line);
    }

    // UAX #9 P2/P3: first strong directional letter sets the paragraph base.
    bool base_rtl = false;
    bool found_strong = false;
    for (char32_t cp : cps) {
        if (is_strong_rtl(cp)) {
            base_rtl = true;
            found_strong = true;
            break;
        }
        if (is_strong_ltr(cp)) {
            base_rtl = false;
            found_strong = true;
            break;
        }
    }
    if (!found_strong || !base_rtl) {
        return sanitize_shaped_utf8(nf::ui::shape_arabic(line));
    }

    // RTL base paragraph: identify embedded LTR tokens (Latin words, file
    // paths/extensions like .nfproj / C:\..., and numbers like 20 / 0.1.0).
    const std::size_t n = cps.size();
    std::vector<bool> ltr(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        const char32_t cp = cps[i];
        if (is_strong_ltr(cp) || nf::ui::is_digit(cp)) {
            ltr[i] = true;
        } else if (cp == U'.' && i + 1 < n && is_strong_ltr(cps[i + 1])) {
            ltr[i] = true; // leading dot in ".nfproj"
        } else if ((cp == U'-' || cp == U'+') && i + 1 < n && nf::ui::is_digit(cps[i + 1]) &&
                   (i == 0 || cps[i - 1] == U' ' || cps[i - 1] == U'(')) {
            ltr[i] = true; // leading sign on "-5"
        } else if ((cp == U'#' || cp == U'+') && i > 0 && ltr[i - 1] && is_strong_ltr(cps[i - 1])) {
            ltr[i] = true; // trailing symbol on "C#" / "C++"
        }
    }

    // Bridge punctuation inside numbers ("0.1.0", "16:9", "1-10", "2D/3D").
    for (std::size_t i = 1; i + 1 < n; ++i) {
        if (!ltr[i] && is_num_bridge(cps[i]) && ltr[i - 1] && ltr[i + 1]) {
            ltr[i] = true;
        }
    }

    // Bridge neutral connectors/spaces between LTR runs when at least one side
    // carries Latin letters (keeps "C:\...\SANAD Projects" and "Visual Studio"
    // intact as one LTR run without gluing separate Arabic-context numbers).
    for (std::size_t i = 0; i < n;) {
        if (!ltr[i]) {
            ++i;
            continue;
        }
        std::size_t left_begin = i;
        bool left_has_latin = false;
        while (i < n && ltr[i]) {
            if (is_strong_ltr(cps[i])) {
                left_has_latin = true;
            }
            ++i;
        }
        std::size_t bridge_begin = i;
        while (i < n && !ltr[i] && is_phrase_bridge(cps[i])) {
            ++i;
        }
        if (i > bridge_begin && i < n && ltr[i]) {
            bool right_has_latin = false;
            for (std::size_t k = i; k < n && ltr[k]; ++k) {
                if (is_strong_ltr(cps[k])) {
                    right_has_latin = true;
                }
            }
            if (left_has_latin || right_has_latin) {
                for (std::size_t k = bridge_begin; k < i; ++k) {
                    ltr[k] = true;
                }
                i = left_begin; // re-scan to chain multiple segments ("C:\a\b c")
                continue;
            }
        }
    }

    struct Run {
        std::size_t begin = 0;
        std::size_t end = 0;
        bool is_ltr = false;
    };
    std::vector<Run> runs;
    for (std::size_t i = 0; i < n;) {
        const bool cur = ltr[i];
        std::size_t j = i + 1;
        while (j < n && ltr[j] == cur) {
            ++j;
        }
        runs.push_back({i, j, cur});
        i = j;
    }

    // Emit runs in RTL visual order (last logical run leftmost, first rightmost).
    std::wstring visual;
    visual.reserve(n);
    for (std::size_t r = runs.size(); r > 0; --r) {
        const Run& run = runs[r - 1];
        if (run.is_ltr) {
            for (std::size_t i = run.begin; i < run.end; ++i) {
                append_gdi_safe(visual, cps[i]);
            }
            continue;
        }
        // RTL run: shape each contiguous Arabic word via shape_arabic(), mirror
        // paired brackets, and reverse the chunk list right-to-left.
        std::vector<std::wstring> chunks;
        for (std::size_t i = run.begin; i < run.end;) {
            if (is_strong_rtl(cps[i]) || is_combining_or_bidi_ctrl(cps[i])) {
                std::size_t j = i + 1;
                while (j < run.end &&
                       (is_strong_rtl(cps[j]) || is_combining_or_bidi_ctrl(cps[j]))) {
                    ++j;
                }
                const std::string word_u8 = encode_u8(cps, i, j);
                chunks.push_back(sanitize_shaped_utf8(nf::ui::shape_arabic(word_u8)));
                i = j;
            } else {
                std::wstring ch;
                append_gdi_safe(ch, mirror_bracket(cps[i]));
                if (!ch.empty()) {
                    chunks.push_back(std::move(ch));
                }
                ++i;
            }
        }
        for (std::size_t c = chunks.size(); c > 0; --c) {
            visual += chunks[c - 1];
        }
    }
    return visual;
}

inline bool has_visual_rtl_forms(const std::wstring& s) {
    for (wchar_t ch : s) {
        const unsigned int u = static_cast<unsigned int>(ch);
        if ((u >= 0xFB50u && u <= 0xFDFFu) || (u >= 0xFE70u && u <= 0xFEFFu)) {
            return true;
        }
    }
    return false;
}

} // namespace gdi_bidi

// Display-ready UTF-16 for HAND-DRAWN text: translate, shape Arabic to visual
// order per line, and sanitize complex-script triggers so ExtTextOutW draws
// clean left-to-right Presentation Forms without Uniscribe double-reversal.
inline std::wstring shell_w_str(const std::string& logical_utf8) {
    if (!nf::ui::needs_shaping(logical_utf8)) {
        return widen(logical_utf8);
    }
    std::wstring out;
    std::size_t begin = 0;
    while (begin <= logical_utf8.size()) {
        std::size_t end = logical_utf8.find('\n', begin);
        if (end == std::string::npos) {
            out += gdi_bidi::shape_line_for_gdi(logical_utf8.substr(begin));
            break;
        }
        out += gdi_bidi::shape_line_for_gdi(logical_utf8.substr(begin, end - begin));
        out.push_back(L'\n');
        begin = end + 1;
    }
    return out;
}

inline std::wstring shell_w_key(const char* key) {
    return shell_w_str(shell_tr(key));
}

/// The real Documents folder. NOT USERPROFILE\Documents: OneDrive (and domain
/// policy) redirects it, and on this machine it is redirected — writing a new
/// project to the un-redirected path would put it somewhere the user never sees.
/// Falls back to the pure rule when the shell call fails.
std::string known_documents_dir() {
    PWSTR raw = nullptr;
    std::string out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &raw)) && raw != nullptr) {
        out = narrow(raw);
    }
    if (raw != nullptr) {
        ::CoTaskMemFree(raw);
    }
    if (!out.empty()) {
        return out;
    }
    const std::string profile = env_var("USERPROFILE");
    if (!profile.empty()) {
        return documents_under(profile);
    }
    return {};
}

std::string recent_store_path() {
    const std::string local = env_var("LOCALAPPDATA");
    if (local.empty()) {
        return {};
    }
    std::filesystem::path p(local);
    p /= "SANAD";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    p /= "recent_projects.txt";
    return p.string();
}

/// True when the process runs below Medium integrity. Browsers stamp
/// downloaded archives with a Low mandatory label, and extraction inherits it;
/// Windows then starts the editor at Low integrity, where Desktop and Documents
/// are read-only. There is no way for such a process to raise its own label
/// (write-up is blocked), so the launcher must explain the cause and keep
/// working from a writable directory instead of failing the Create button.
bool process_is_low_integrity() {
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    DWORD len = 0;
    ::GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &len);
    bool low = false;
    if (len != 0) {
        std::vector<unsigned char> buffer(len);
        if (::GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), len, &len) != FALSE) {
            auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
            if (label->Label.Sid != nullptr) {
                const DWORD count = *::GetSidSubAuthorityCount(label->Label.Sid);
                if (count > 0) {
                    const DWORD rid = *::GetSidSubAuthority(label->Label.Sid, count - 1);
                    low = rid < SECURITY_MANDATORY_MEDIUM_RID;
                }
            }
        }
    }
    ::CloseHandle(token);
    return low;
}

/// Creates and removes a uniquely named directory under `dir`. This is the
/// exact operation Create needs (mkdir), so a location that passes here is a
/// location scaffold_project can write to.
bool directory_is_writable(const std::string& dir) {
    if (dir.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return false;
    }
    const std::filesystem::path probe =
        std::filesystem::path(dir) /
        (".sanad_write_probe_" + std::to_string(::GetCurrentProcessId()));
    if (!std::filesystem::create_directory(probe, ec) || ec) {
        return false;
    }
    std::error_code removed;
    std::filesystem::remove(probe, removed);
    return true;
}

/// Writable home for new projects when the Documents default is not usable
/// (Low-integrity editor). AppData\LocalLow is Windows' designated write
/// location for Low IL, so it works even when the whole profile is Medium. The
/// module's Projects/ folder is the fallback for a tree that is itself Low
/// (the raw downloaded ZIP case).
std::string fallback_project_dir() {
    std::vector<std::filesystem::path> candidates;
    if (const std::string profile = env_var("USERPROFILE"); !profile.empty()) {
        candidates.emplace_back(std::filesystem::path(profile) / "AppData" / "LocalLow" /
                                "SANAD Projects");
    }
    const std::filesystem::path module = launcher_module_dir();
    if (!module.empty()) {
        candidates.emplace_back(module / "Projects");
    }
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code ec;
        std::filesystem::create_directories(candidate, ec);
        if (ec) {
            continue;
        }
        const std::string text = candidate.string();
        if (directory_is_writable(text)) {
            return text;
        }
    }
    return {};
}

// Defined with the gallery table below; declared here because
// create_project_in_shell() — which comes first — calls it.
std::string selected_template_dir(int index, const char* templates_root,
                                  std::string& out_error);

// --- control ids -------------------------------------------------------------
// Native controls are used only for text input (search, name, location);
// navigation and cards are hit-tested manually so the shell stays a single
// paint routine instead of a hundred child windows.
enum : int {
    kIdList = 1001, // (legacy listbox id, unused by the shell — kept stable)
    kIdOpen = 1002,
    kIdNewName = 1003,
    kIdNewDir = 1004,
    kIdBrowse = 1005,
    kIdCreate = 1006,
    kIdQuit = 1007,
    kIdStatus = 1008,
    kIdEmpty = 1009,
    kIdSecRecent = 1010,
    kIdSecCreate = 1011,
    kIdFootnote = 1012,
    kIdSearch = 1013, // Projects-page filter box
    kIdLangEn = 1014, // Settings: English radio
    kIdLangAr = 1015, // Settings: Arabic radio
    kIdClearRecent = 1016, // Settings: forget recent projects
};

// Shell pages, matching the sidebar in the product mockups.
enum class ShellPage : int {
    Projects = 0,
    NewProject = 1,
    Learn = 2,
    AssetStore = 3,
    Settings = 4,
    Help = 5,
};

// A decoded PNG thumbnail (GDI+ → HBITMAP, 32bpp). Missing files are NOT an
// error: the card falls back to a gradient block, so the shell also works
// from install layouts without the Docs tree.
struct Thumb {
    HBITMAP bmp = nullptr;
    int w = 0;
    int h = 0;
};

// Button visual roles, carried in GWLP_USERDATA of each button.
enum class BtnRole : int {
    primary = 0, // filled accent — Create project
    secondary = 1, // outlined — Open project file
    ghost = 2, // quiet surface — Quit / Browse
};

struct BtnExtra {
    BtnRole role = BtnRole::ghost;
    HFONT font = nullptr;
    bool hover = false;
};

// Hover prop key on a subclassed button HWND.
constexpr const wchar_t* kHoverProp = L"NFBtnHover";

struct LauncherState {
    const std::vector<std::string>* recent = nullptr;
    LauncherResult result;
    std::string new_dir;
    bool done = false;

    UINT dpi = 96;
    HFONT font_body = nullptr;    // Segoe UI 10
    HFONT font_small = nullptr;   // Segoe UI 9
    HFONT font_title = nullptr;   // Amiri 20 display (fallback Segoe UI)
    HFONT font_section = nullptr; // Segoe UI Semibold 11
    HFONT font_btn = nullptr;     // Segoe UI 10
    HFONT font_side = nullptr;    // Segoe UI 10 (sidebar)
    HFONT font_big = nullptr;     // Amiri 26 display (fallback Segoe UI)
    HFONT font_sym = nullptr;     // Segoe UI 26 symbols (emoji cards)

    HBRUSH brush_bg = nullptr;
    HBRUSH brush_surface = nullptr;
    HBRUSH brush_list = nullptr;
    HBRUSH brush_sel = nullptr;

    COLORREF status_color = theme::text_dim;
    // The status line's text, cached for owner drawing (SS_OWNERDRAW asks the
    // parent to paint the STATIC itself — see draw_status).
    std::wstring status_text;
    bool list_empty = false;
    int sep_y = 0; // logical client Y of the mid-pane rule (drawn in WM_PAINT)

    // --- shell state ------------------------------------------------------
    ShellPage page = ShellPage::Projects;
    int hover_nav = -1;   // sidebar index under the cursor, -1 = none
    int hover_card = -1;  // project/template card index under cursor
    int hover_btn = -1;   // page-local button index under cursor
    // True when Windows started the editor below Medium integrity. A ZIP
    // downloaded by a browser carries a Low mandatory label; extraction
    // inherits it, and a Low-IL process may not write to Desktop/Documents.
    bool low_integrity = false;
    // First writable directory found when the default location is read-only
    // (empty when Documents was usable). Shown in the New Project form.
    std::string fallback_dir;
    std::string search;   // Projects filter (mirrors the search EDIT)
    int selected_recent = 0; // featured project = filtered[selected_recent]
    // ThirdPerson (index 3), not the old "blank" default: a first project that
    // already contains a ground, a controllable character and crates answers
    // "how do I make a game" better than an empty scene does.
    int selected_template = 3; // template gallery index (3 = ThirdPerson)
    int project_dim = 0; // 0 = All/Both (2D+3D), 1 = 2D, 2 = 3D
    bool arabic_ui = false;  // Settings choice, returned in result
    // Thumbnails: engine screenshots shipped in Docs/images, resolved at
    // startup by walking up from the module path. Order: game, vehicle,
    // editor_en, editor_ar. Empty entries = missing file (gradient fallback).
    Thumb thumbs[4];
    // The brand mark (Editor/resources/SANAD.png, or SANAD.png beside the exe
    // in a package). Drawn in the sidebar header; null = text-only header.
    Thumb logo;
    ULONG_PTR gdiplus_token = 0;
};

LauncherState* state_of(HWND hwnd) {
    return reinterpret_cast<LauncherState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int scale_dpi(const LauncherState* st, int px) {
    const UINT dpi = (st != nullptr && st->dpi != 0) ? st->dpi : 96u;
    return ::MulDiv(px, static_cast<int>(dpi), 96);
}

HFONT make_font(int pt, bool semibold, UINT dpi) {
    const int height = -::MulDiv(pt, static_cast<int>(dpi), 72);
    return ::CreateFontW(height, 0, 0, 0, semibold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

// Display face (Amiri) for headings/wordmark: authentic Arabic typography for
// the big strings, Segoe UI everywhere else. Loaded privately from the shipped
// Resources/fonts tree (same walk-up as the thumbnails); absence falls back
// to Segoe UI silently — the shell never depends on art files to run.
std::wstring g_amiri_path;
bool g_amiri_ok = false;

std::wstring find_font_file() {
    wchar_t mod[MAX_PATH * 2] = {};
    DWORD n = ::GetModuleFileNameW(nullptr, mod, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) {
        return {};
    }
    std::filesystem::path dir(mod);
    dir = dir.parent_path();
    for (int i = 0; i < 8; ++i) {
        std::error_code ec;
        const std::filesystem::path cand = dir / "Resources" / "fonts" / "Amiri-Regular.ttf";
        if (std::filesystem::exists(cand, ec)) {
            return cand.wstring();
        }
        if (!dir.has_parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }
    return {};
}

HFONT make_font_face(int pt, bool semibold, UINT dpi, bool display) {
    const int height = -::MulDiv(pt, static_cast<int>(dpi), 72);
    const wchar_t* face = (display && g_amiri_ok) ? L"Amiri" : L"Segoe UI";
    return ::CreateFontW(height, 0, 0, 0, semibold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

// Shell font set: built once at startup, rebuilt when the window moves to a
// monitor with a different DPI (see refresh_dpi). Fonts are never selected
// into a live DC outside a paint call, so delete/recreate between messages is
// safe — but every rebuild must re-send WM_SETFONT to the native children,
// which cache their HFONT.
void destroy_state_fonts(LauncherState* st) {
    if (st == nullptr) {
        return;
    }
    HFONT* fonts[] = {&st->font_body, &st->font_small, &st->font_title, &st->font_section,
                      &st->font_btn, &st->font_side, &st->font_big, &st->font_sym};
    for (HFONT* f : fonts) {
        if (*f != nullptr) {
            ::DeleteObject(*f);
            *f = nullptr;
        }
    }
}

void create_state_fonts(LauncherState* st) {
    if (st == nullptr) {
        return;
    }
    destroy_state_fonts(st); // rebuild path: drop the old set first
    st->font_body = make_font(10, false, st->dpi);
    st->font_small = make_font(9, false, st->dpi);
    st->font_title = make_font_face(20, true, st->dpi, true); // display face
    st->font_section = make_font(11, true, st->dpi);
    st->font_btn = make_font(10, false, st->dpi);
    st->font_side = make_font(10, false, st->dpi);
    st->font_big = make_font_face(26, true, st->dpi, true); // display face
    st->font_sym = make_font(26, true, st->dpi);            // symbols stay Segoe
}

// Re-sends the (possibly rebuilt) fonts to the native children.
void restyle_children(HWND hwnd, const LauncherState* st) {
    struct Item {
        int id;
        HFONT font;
    };
    const Item items[] = {{kIdSearch, st->font_body},
                          {kIdNewName, st->font_body},
                          {kIdNewDir, st->font_body},
                          {kIdStatus, st->font_small}};
    for (const Item& it : items) {
        if (HWND h = ::GetDlgItem(hwnd, it.id)) {
            ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(it.font), TRUE);
        }
    }
}

// DPI of the monitor the window currently lives on (not the primary screen:
// the shell opens centered on the primary but the user can drag it anywhere).
// Called on WM_SIZE/WM_MOVE; rebuilds fonts only when the value CHANGED, so
// resize drags stay cheap. (The manifest is system-DPI-aware, so no
// WM_DPICHANGED arrives — polling here is the documented pattern for that
// level. A WM_DPICHANGED handler below covers PerMonitor-aware futures.)
void refresh_dpi(HWND hwnd, LauncherState* st) {
    if (st == nullptr) {
        return;
    }
    UINT dpi = 96;
    if (HDC dc = ::GetDC(hwnd)) {
        const int got = ::GetDeviceCaps(dc, LOGPIXELSX);
        if (got > 0) {
            dpi = static_cast<UINT>(got);
        }
        ::ReleaseDC(hwnd, dc);
    }
    if (dpi != st->dpi) {
        st->dpi = dpi;
        create_state_fonts(st);
        restyle_children(hwnd, st);
    }
}

void set_status(HWND hwnd, const std::string& text, COLORREF color = theme::error) {
    if (LauncherState* st = state_of(hwnd)) {
        st->status_color = color;
        st->status_text = shell_w_str(text); // owner-draw reads the cache (draw_status)
    }
    ::SetDlgItemTextW(hwnd, kIdStatus, widen(text).c_str());
    if (HWND h = ::GetDlgItem(hwnd, kIdStatus)) {
        ::InvalidateRect(h, nullptr, TRUE);
    }
}

/// Builds the recent list slots. Owner-draw paints name + path from
/// `state.recent` (item data = index), so the string in each slot is only a
/// placeholder. Zero entries hides the list and shows the empty-state hint.
void fill_list_slots(HWND hwnd, const std::vector<std::string>& recent) {
    HWND list = ::GetDlgItem(hwnd, kIdList);
    HWND empty = ::GetDlgItem(hwnd, kIdEmpty);
    ::SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (std::size_t i = 0; i < recent.size(); ++i) {
        ::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L""));
        ::SendMessageW(list, LB_SETITEMDATA, static_cast<WPARAM>(i), static_cast<LPARAM>(i));
    }
    LauncherState* st = state_of(hwnd);
    const bool is_empty = recent.empty();
    if (st != nullptr) {
        st->list_empty = is_empty;
    }
    ::ShowWindow(list, is_empty ? SW_HIDE : SW_SHOW);
    if (empty != nullptr) {
        ::ShowWindow(empty, is_empty ? SW_SHOW : SW_HIDE);
    }
    if (!is_empty) {
        ::SendMessageW(list, LB_SETCURSEL, 0, 0);
    }
}

/// Opens a picked path, or reports why it cannot be opened. A path that does not
/// exist is the common case for a stale recent entry, so it says so rather than
/// silently doing nothing.
void accept_project(HWND hwnd, const std::string& path) {
    LauncherState* st = state_of(hwnd);
    if (st == nullptr) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        // Native STATIC control: logical text (the control shapes natively).
        set_status(hwnd, shell_tr("sh_st_missing") + "\n" + path);
        return;
    }
    st->result.project_path = path;
    st->result.quit = false;
    st->done = true;
    ::PostQuitMessage(0);
}

void on_create(HWND hwnd) {
    LauncherState* st = state_of(hwnd);
    if (st == nullptr) {
        return;
    }
    wchar_t name_buf[128] = {};
    ::GetDlgItemTextW(hwnd, kIdNewName, name_buf, 128);
    const std::string name = narrow(name_buf);

    const std::string name_err = project_name_error(name);
    if (!name_err.empty()) {
        // A localization KEY (err_name_*) since the header change — translate
        // at display time so the Arabic UI never shows English reasons.
        set_status(hwnd, shell_tr(name_err.c_str()));
        return;
    }

    std::string dir = st->new_dir;
    if (dir.empty()) {
        set_status(hwnd, shell_tr("sh_st_choose_loc"));
        return;
    }

    const std::filesystem::path target = std::filesystem::path(dir) / name;
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        set_status(hwnd, shell_tr("sh_st_exists") + "\n" + target.string());
        return;
    }

    nf::project::ScaffoldOptions so;
    so.root = target.string();
    so.name = name;
    // Without the template the project has an empty Content/ and no
    // Main.nfscene, so the editor exits as soon as it opens — the same
    // NF_TEMPLATE_DIR the in-editor New Project path uses (main.cpp).
#ifndef NF_TEMPLATE_DIR
#define NF_TEMPLATE_DIR ""
#endif
#ifndef NF_TEMPLATES_ROOT
#define NF_TEMPLATES_ROOT ""
#endif
    // The gallery card the user actually chose, resolved through the SAME
    // allow-list the CLI uses. A placeholder or out-of-range index yields the
    // Default template rather than a project with no scene to open.
    std::string create_err;
    so.template_dir = selected_template_dir(st->selected_template, NF_TEMPLATES_ROOT,
                                             create_err);
    if (so.template_dir.empty()) {
        // A named template whose folder is missing must not silently scaffold an
        // empty project: say which template is broken and stop.
        NF_LOG_ERROR(LogCategory::Editor, "launcher template error: {}", create_err);
        set_status(hwnd, shell_tr("sh_st_create_fail") + "\n" + create_err);
        return;
    }
    std::string perr;
    if (!nf::project::scaffold_project(so, perr)) {
        NF_LOG_ERROR(LogCategory::Editor, "launcher scaffold error: {}", perr);
        // Access denied under Low integrity is not a generic permission bug:
        // Windows is enforcing the download label, and the remedy is the
        // repository's FixPermissions.cmd, not "pick another folder" alone.
        if (st->low_integrity && perr.find("(code 5)") != std::string::npos) {
            set_status(hwnd, shell_tr("sh_st_create_fail") + "\n" + perr + "\n" +
                                 shell_tr("sh_st_low_integrity_hint"));
        } else {
            set_status(hwnd, shell_tr("sh_st_create_fail") + "\n" + perr);
        }
        return;
    }

    // scaffold_project writes <root>/<name>.nfproj; hand that back so the caller
    // takes the ordinary --project path from here.
    const std::string proj = project_file_for(target.string(), name);
    if (!std::filesystem::exists(proj, ec)) {
        set_status(hwnd, shell_tr("sh_st_nfproj_missing") + "\n" + proj);
        return;
    }
    st->result.project_path = proj;
    st->result.quit = false;
    st->done = true;
    ::PostQuitMessage(0);
}

// The New Project gallery table lives in the header (kLauncherTemplateCards) so
// EditorTests can pin the two invariants that keep it honest — every enabled
// card names a real template, and every real template has an enabled card.
// Aliased here because every call site in this file reads better as `kTemplates`.
constexpr const auto& kTemplates = kLauncherTemplateCards;

/// The template folder for gallery card `index`, or "" with `out_error` set.
///
/// Three cases, each falling back for a different reason:
///
///  - A real card   -> the folder named by the table, resolved through the CLI's
///                     own allow-list so the two cannot drift.
///  - A placeholder -> Default, because scaffolding with no template yields a
///                     Content/ with no Main.nfscene and the editor exits on
///                     opening it. A greyed-out card must never be able to
///                     produce an unusable project.
///  - Out of range  -> Default, for that reason and because it means the state
///                     and the table have drifted apart.
///
/// `out_error` is set only when a NAMED template could not be resolved, which is
/// a broken build (the folder is gone), not a user mistake.
std::string selected_template_dir(int index, const char* templates_root,
                                  std::string& out_error) {
    out_error.clear();
    const char* requested = "Default";
    if (index >= 0 && index < static_cast<int>(std::size(kTemplates)) &&
        kTemplates[index].enabled && kTemplates[index].template_name != nullptr &&
        kTemplates[index].template_name[0] != '\0') {
        requested = kTemplates[index].template_name;
    }
    // Root vs default-dir, exactly as the CLI passes them (Tools/BuildTool):
    // NF_TEMPLATES_ROOT is the tree the named templates live under,
    // NF_TEMPLATE_DIR is only the Default template's folder. Substituting one
    // for the other made every gallery card resolve to a folder that does not
    // exist (Templates/Default/<Name>), so the create always failed.
    //
    // The compile-time root is an ABSOLUTE path in the developer's build tree.
    // In the portable demo that path does not exist, so every card failed with
    // "template not available" — the package could not create a project at
    // all. Candidates are tried in order; the first one that actually contains
    // the requested template wins:
    //   1. SANAD_TEMPLATES_ROOT (explicit override, packaging/QA)
    //   2. NF_TEMPLATES_ROOT (developer tree)
    //   3. <exe>/Templates          (portable zip: editor beside Templates/)
    //   4. <exe>/../Templates       (editor under bin/)
    //   5. the Default folder's parent (stub-build guard)
    std::vector<std::filesystem::path> roots;
    const auto add_root = [&roots](std::filesystem::path candidate) {
        if (candidate.empty()) {
            return;
        }
        for (const auto& seen : roots) {
            if (seen == candidate) {
                return;
            }
        }
        roots.push_back(std::move(candidate));
    };
    if (const std::string env = env_var("SANAD_TEMPLATES_ROOT"); !env.empty()) {
        add_root(env);
    }
    if (templates_root != nullptr && templates_root[0] != '\0') {
        add_root(templates_root);
    }
    if (const std::filesystem::path module = launcher_module_dir(); !module.empty()) {
        add_root(module / "Templates");
        add_root(module.parent_path() / "Templates");
    }
    add_root(std::filesystem::path(NF_TEMPLATE_DIR).parent_path());
    if (roots.empty()) {
        roots.emplace_back(std::filesystem::current_path());
    }

    // Selection order lives in the header so EditorTests can pin it: a package
    // must never resolve against a build-tree path that only exists on the
    // developer's machine.
    const std::filesystem::path root = nf::project::choose_template_root(roots, requested);
    const auto resolved = nf::project::resolve_project_template(
        requested, root, {}, out_error);
    return resolved.string();
}

// --- owner-draw painting -----------------------------------------------------

void fill_round(HDC dc, const RECT& rc, COLORREF color, int radius) {
    HBRUSH br = ::CreateSolidBrush(color);
    HPEN pen = ::CreatePen(PS_SOLID, 1, color);
    HBRUSH old_br = static_cast<HBRUSH>(::SelectObject(dc, br));
    HPEN old_pen = static_cast<HPEN>(::SelectObject(dc, pen));
    ::RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    ::SelectObject(dc, old_br);
    ::SelectObject(dc, old_pen);
    ::DeleteObject(br);
    ::DeleteObject(pen);
}

void frame_round(HDC dc, const RECT& rc, COLORREF color, int radius, int width = 1) {
    HPEN pen = ::CreatePen(PS_SOLID, width, color);
    HBRUSH old_br = static_cast<HBRUSH>(::SelectObject(dc, ::GetStockObject(NULL_BRUSH)));
    HPEN old_pen = static_cast<HPEN>(::SelectObject(dc, pen));
    ::RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    ::SelectObject(dc, old_br);
    ::SelectObject(dc, old_pen);
    ::DeleteObject(pen);
}

// DrawTextW is unreliable on some Windows sessions: it returns a line height
// and leaves the surface untouched. ExtTextOutW is the primitive underneath
// TextOut and keeps working there, so every hand-drawn string goes through this
// helper. It reproduces the small set of DT_* features the shell uses:
// single line, left/center/right, vertical centering, end ellipsis and word
// wrap. Arabic has already been converted to visual order by shell_w_str;
// this helper only draws left-to-right glyph runs.
void draw_text_ext(HDC dc, const wchar_t* text, int x, int y, int w, int h, HFONT font,
                   COLORREF color, UINT fmt) {
    if (dc == nullptr || text == nullptr || text[0] == L'\0' || w <= 0 || h <= 0) {
        return;
    }
    const int saved = ::SaveDC(dc);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, color);
    ::SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    if (font != nullptr) {
        ::SelectObject(dc, font);
    }
    const RECT clip{x, y, x + w, y + h};
    // ETO_RTLREADING makes ExtTextOutW drop the whole string on this Windows
    // session (the same failure family as DrawTextW). Alignment is mirrored by
    // paint_text instead; Arabic visual order is handled before this call.
    const UINT eto = ETO_CLIPPED;

    const auto extent = [&](const wchar_t* s, int len) {
        SIZE sz{};
        ::GetTextExtentPoint32W(dc, s, len, &sz);
        return sz;
    };
    const auto draw_line = [&](const std::wstring& line, int top, bool vcenter) {
        if (line.empty()) {
            return;
        }
        const SIZE sz = extent(line.c_str(), static_cast<int>(line.size()));
        int tx = x;
        if ((fmt & DT_CENTER) != 0) {
            tx = x + (w - sz.cx) / 2;
        } else if ((fmt & DT_RIGHT) != 0) {
            tx = x + w - sz.cx;
        }
        const int ty = vcenter ? top + (h - sz.cy) / 2 : top;
        // Clip horizontally to the box, but never cut the glyph cell
        // vertically. Amiri (the display face) has a tall ascent+descent, so a
        // 26 pt heading's cell is taller than the layout boxes it is centred
        // in; clipping to the box sliced off every descender - "مشروع" lost
        // the tails of ر/و/ع and "projects yet" lost p/j/y. Horizontal
        // overflow is still clipped (end-ellipsis already fits it to w).
        const RECT line_clip{x, std::min(y, ty), x + w,
                             std::max(y + h, ty + static_cast<int>(sz.cy))};
        ::ExtTextOutW(dc, tx, ty, eto, &line_clip, line.c_str(),
                      static_cast<UINT>(line.size()), nullptr);
    };

    std::wstring all(text);
    if ((fmt & DT_SINGLELINE) != 0 || (fmt & DT_WORDBREAK) == 0) {
        const SIZE sz = extent(all.c_str(), static_cast<int>(all.size()));
        if ((fmt & DT_END_ELLIPSIS) != 0 && sz.cx > w) {
            constexpr wchar_t kEllipsis = L'\u2026';
            const SIZE ell = extent(&kEllipsis, 1);
            // In visual-order RTL strings, the start of the sentence is on the
            // RIGHT (all.back()) and the end is on the LEFT (all.front()).
            // Popping from the back truncated the FIRST Arabic words instead
            // of the trailing ones.
            const bool rtl_vis = gdi_bidi::has_visual_rtl_forms(all);
            while (!all.empty()) {
                const SIZE cur = extent(all.c_str(), static_cast<int>(all.size()));
                if (cur.cx + ell.cx <= w) {
                    break;
                }
                if (rtl_vis) {
                    all.erase(0, 1);
                } else {
                    all.pop_back();
                }
            }
            if (rtl_vis) {
                all.insert(0, 1, kEllipsis);
            } else {
                all.push_back(kEllipsis);
            }
        }
        draw_line(all, y, (fmt & DT_VCENTER) != 0);
        ::RestoreDC(dc, saved);
        return;
    }

    // Word wrap: split paragraphs on newlines, then words on spaces. A single
    // word wider than the box is drawn clipped rather than dropped. Visual-RTL
    // paragraphs carry their first reading-order words on the RIGHT end, so
    // wrap them right-to-left to keep top-to-bottom Arabic reading order.
    const SIZE metrics = extent(L"Ag", 2);
    const int line_h = metrics.cy > 0 ? metrics.cy : h;
    int top = y;
    std::size_t begin = 0;
    while (begin <= all.size() && top < y + h) {
        std::size_t end = begin;
        while (end < all.size() && all[end] != L'\n') {
            ++end;
        }
        const std::wstring paragraph = all.substr(begin, end - begin);
        const bool rtl_para = gdi_bidi::has_visual_rtl_forms(paragraph);
        std::wstring line;
        if (rtl_para) {
            std::size_t j = paragraph.size();
            while (j > 0) {
                while (j > 0 && paragraph[j - 1] == L' ') {
                    --j;
                }
                const std::size_t word_end = j;
                while (j > 0 && paragraph[j - 1] != L' ') {
                    --j;
                }
                if (j == word_end) {
                    break;
                }
                const std::wstring word = paragraph.substr(j, word_end - j);
                const std::wstring candidate = line.empty() ? word : (word + L" " + line);
                const SIZE candidate_size =
                    extent(candidate.c_str(), static_cast<int>(candidate.size()));
                if (!line.empty() && candidate_size.cx > w) {
                    draw_line(line, top, false);
                    top += line_h;
                    line = word;
                    if (top >= y + h) {
                        break;
                    }
                } else {
                    line = candidate;
                }
            }
        } else {
            std::size_t i = 0;
            while (i < paragraph.size()) {
                while (i < paragraph.size() && paragraph[i] == L' ') {
                    ++i;
                }
                const std::size_t word_begin = i;
                while (i < paragraph.size() && paragraph[i] != L' ') {
                    ++i;
                }
                if (word_begin == i) {
                    break;
                }
                const std::wstring word = paragraph.substr(word_begin, i - word_begin);
                const std::wstring candidate = line.empty() ? word : (line + L" " + word);
                const SIZE candidate_size =
                    extent(candidate.c_str(), static_cast<int>(candidate.size()));
                if (!line.empty() && candidate_size.cx > w) {
                    draw_line(line, top, false);
                    top += line_h;
                    line = word;
                    if (top >= y + h) {
                        break;
                    }
                } else {
                    line = candidate;
                }
            }
        }
        if (!line.empty() && top < y + h) {
            draw_line(line, top, false);
            top += line_h;
        }
        if (end >= all.size()) {
            break;
        }
        begin = end + 1;
    }
    ::RestoreDC(dc, saved);
}

void draw_button(const DRAWITEMSTRUCT* dis, const BtnExtra* ex) {
    RECT rc = dis->rcItem;
    const int radius = ::MulDiv(6, 96, 96); // 6px logical
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool hover = ex != nullptr && ex->hover;
    const BtnRole role = ex != nullptr ? ex->role : BtnRole::ghost;

    COLORREF fill = theme::surface;
    COLORREF line = theme::border;
    COLORREF label = theme::text;

    switch (role) {
        case BtnRole::primary:
            fill = pressed ? theme::accent_lo : (hover ? theme::accent_hi : theme::accent);
            line = fill;
            label = theme::on_accent;
            break;
        case BtnRole::secondary:
            fill = pressed ? theme::sel_bg : (hover ? theme::surface_hi : theme::surface);
            line = hover ? theme::accent : theme::border;
            label = hover ? theme::accent : theme::text;
            break;
        case BtnRole::ghost:
        default:
            fill = pressed ? theme::bg : (hover ? theme::surface_hi : theme::surface);
            line = hover ? theme::border : theme::bg;
            label = hover ? theme::text : theme::text_dim;
            break;
    }
    if (disabled) {
        fill = theme::bg;
        line = theme::border;
        label = theme::text_dim;
    }

    // The button class already erased; paint the rounded body over it.
    fill_round(dis->hDC, rc, fill, radius);
    if (role != BtnRole::primary) {
        frame_round(dis->hDC, rc, line, radius);
    }

    wchar_t text[128] = {};
    ::GetWindowTextW(dis->hwndItem, text, 128);

    const std::wstring shaped = shell_w_str(narrow(text));
    draw_text_ext(dis->hDC, shaped.c_str(), rc.left, rc.top, rc.right - rc.left,
                  rc.bottom - rc.top, ex != nullptr ? ex->font : nullptr, label,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if ((dis->itemState & ODS_FOCUS) != 0) {
        RECT focus_rc = rc;
        ::InflateRect(&focus_rc, -3, -3);
        ::DrawFocusRect(dis->hDC, &focus_rc);
    }
}

// Recent-list row: project name on top, full path beneath, dimmed.
void draw_list_item(const DRAWITEMSTRUCT* dis, const LauncherState* st) {
    const std::size_t idx = static_cast<std::size_t>(dis->itemID);
    std::string path;
    if (st != nullptr && st->recent != nullptr && idx < st->recent->size()) {
        path = (*st->recent)[idx];
    }
    const std::string name =
        path.empty() ? std::string{} : std::filesystem::path(path).stem().string();

    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    const bool focused = (dis->itemState & ODS_FOCUS) != 0;

    RECT rc = dis->rcItem;
    HBRUSH bg = ::CreateSolidBrush(selected ? theme::sel_bg : theme::list_bg);
    ::FillRect(dis->hDC, &rc, bg);
    ::DeleteObject(bg);

    if (selected) {
        RECT bar = rc;
        bar.right = bar.left + ::MulDiv(3, 96, 96);
        HBRUSH accent = ::CreateSolidBrush(theme::sel_bar);
        ::FillRect(dis->hDC, &bar, accent);
        ::DeleteObject(accent);
    }

    if (path.empty()) {
        return;
    }

    const int pad_l = scale_dpi(st, 14);
    const int pad_t = scale_dpi(st, 6);
    const int pad_r = scale_dpi(st, 12);

    // Name (row 1)
    RECT name_rc = {rc.left + pad_l, rc.top + pad_t, rc.right - pad_r,
                    rc.top + pad_t + scale_dpi(st, 18)};
    const UINT row_fmt =
        (g_shell_rtl ? DT_RIGHT : DT_LEFT) | DT_SINGLELINE | DT_END_ELLIPSIS;
    const std::wstring wname = shell_w_str(name);
    draw_text_ext(dis->hDC, wname.c_str(), name_rc.left, name_rc.top,
                  name_rc.right - name_rc.left, name_rc.bottom - name_rc.top,
                  st != nullptr ? st->font_body : nullptr, theme::text, row_fmt);

    // Path (row 2)
    RECT path_rc = {rc.left + pad_l, name_rc.bottom + scale_dpi(st, 2), rc.right - pad_r,
                    rc.bottom - scale_dpi(st, 4)};
    const std::wstring wpath = shell_w_str(path);
    draw_text_ext(dis->hDC, wpath.c_str(), path_rc.left, path_rc.top,
                  path_rc.right - path_rc.left, path_rc.bottom - path_rc.top,
                  st != nullptr ? st->font_small : nullptr, theme::text_dim, row_fmt);

    if (focused && !selected) {
        ::DrawFocusRect(dis->hDC, &rc);
    }
}

// --- shell: thumbnails, helpers, pages ---------------------------------------
// The product shell (sidebar + pages) lives here. It reuses every behaviour
// above (recent list, scaffold, pickers, theme) and only changes presentation:
// one window, left nav, per-page content, PNG thumbnails decoded with GDI+
// (GPU-free: the shell runs before the device exists).

namespace shell {

// Engine version shown in the sidebar footer, project cards and hero line.
// Derived from the root project(VERSION) via the NF_ENGINE_VERSION compile
// definition — NOT a hand-maintained literal. (The previous literal here went
// stale after any build that did not remember to edit it; the launcher kept
// advertising an old version. The define is single-source and cannot drift.)
#ifndef NF_ENGINE_VERSION
#define NF_ENGINE_VERSION "0.0.0-dev"
#endif
constexpr const char* kEngineVersion = NF_ENGINE_VERSION;
constexpr const char* kRendererName = "Vulkan";

/// Locate Docs/images/<file> by walking up from the module directory (the
/// launcher may run from build/bin, the repo root, or an install dir).
/// Empty when absent — the caller draws a gradient block instead.
std::wstring find_doc_image(const wchar_t* file) {
    wchar_t mod[MAX_PATH * 2] = {};
    DWORD n = ::GetModuleFileNameW(nullptr, mod, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) {
        return {};
    }
    std::filesystem::path dir(mod);
    dir = dir.parent_path();
    for (int i = 0; i < 8; ++i) {
        std::error_code ec;
        std::filesystem::path cand = dir / "Docs" / "images" / file;
        if (std::filesystem::exists(cand, ec)) {
            return cand.wstring();
        }
        if (!dir.has_parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }
    return {};
}

/// Decode a PNG file to a 32bpp top-down HBITMAP. Null on any failure.
HBITMAP load_thumb(const std::wstring& path, int& out_w, int& out_h) {
    out_w = 0;
    out_h = 0;
    if (path.empty()) {
        return nullptr;
    }
    Gdiplus::Bitmap bmp(path.c_str(), FALSE);
    if (bmp.GetLastStatus() != Gdiplus::Ok) {
        return nullptr;
    }
    const UINT w = bmp.GetWidth();
    const UINT h = bmp.GetHeight();
    if (w == 0 || h == 0 || w > 4096 || h > 4096) {
        return nullptr;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h); // top-down: row 0 is the top
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbmp = ::CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbmp == nullptr || bits == nullptr) {
        if (hbmp != nullptr) {
            ::DeleteObject(hbmp);
        }
        return nullptr;
    }
    Gdiplus::BitmapData data{};
    Gdiplus::Rect rc(0, 0, static_cast<INT>(w), static_cast<INT>(h));
    // PixelFormat32bppPARGB matches the premultiplied AlphaBlend path below.
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) !=
        Gdiplus::Ok) {
        ::DeleteObject(hbmp);
        return nullptr;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(w) * 4u;
    for (UINT y = 0; y < h; ++y) {
        std::memcpy(static_cast<unsigned char*>(bits) + y * row_bytes,
                    static_cast<const unsigned char*>(data.Scan0) + y * data.Stride, row_bytes);
    }
    bmp.UnlockBits(&data);
    out_w = static_cast<int>(w);
    out_h = static_cast<int>(h);
    return hbmp;
}

/// Locate the brand mark. A package ships it beside the exe (SANAD.png, the
/// dist layout) or under Resources/; a source-tree run finds the committed
/// Editor/resources/SANAD.png by walking up. Empty when absent.
std::wstring find_logo_image() {
    wchar_t mod[MAX_PATH * 2] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, mod, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) {
        return {};
    }
    std::filesystem::path dir = std::filesystem::path(mod).parent_path();
    for (int i = 0; i < 8; ++i) {
        std::error_code ec;
        for (const std::filesystem::path& cand :
             {dir / "SANAD.png", dir / "Resources" / "SANAD.png",
              dir / "Editor" / "resources" / "SANAD.png"}) {
            if (std::filesystem::exists(cand, ec)) {
                return cand.wstring();
            }
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            break;
        }
        dir = dir.parent_path();
    }
    return {};
}

/// Draws the brand mark into a rounded square. Unlike paint_thumb this keeps
/// the whole image (no crop, no title-bar skip) and filters with HALFTONE, so
/// a 256 px logo shrunk to ~36 px stays smooth instead of aliased.
void paint_logo(HDC dc, const Thumb& th, int x, int y, int s, int radius) {
    if (th.bmp == nullptr || th.w <= 0 || th.h <= 0 || s <= 0) {
        return;
    }
    HDC mem = ::CreateCompatibleDC(dc);
    HGDIOBJ old = ::SelectObject(mem, th.bmp);
    const int saved = ::SaveDC(dc);
    HRGN rgn = ::CreateRoundRectRgn(x, y, x + s + 1, y + s + 1, radius, radius);
    ::SelectClipRgn(dc, rgn);
    ::SetStretchBltMode(dc, HALFTONE);
    ::SetBrushOrgEx(dc, 0, 0, nullptr);
    ::StretchBlt(dc, x, y, s, s, mem, 0, 0, th.w, th.h, SRCCOPY);
    ::RestoreDC(dc, saved);
    ::DeleteObject(rgn);
    ::SelectObject(mem, old);
    ::DeleteDC(mem);
    frame_round(dc, {x, y, x + s, y + s}, theme::border, radius);
}

/// Human "edited" stamp from an .nfproj mtime ("2 hours ago"). Pure string
/// math around filesystem time; empty when the file is missing. Buckets are
/// localization keys (sh_ago_*) so the stamp translates with the shell.
std::string edited_ago(const std::string& nfproj_path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(nfproj_path, ec);
    if (ec) {
        return {};
    }
    const auto now = std::filesystem::file_time_type::clock::now();
    long long mins =
        std::chrono::duration_cast<std::chrono::minutes>(now - t).count();
    if (mins < 0) {
        mins = 0;
    }
    char buf[64] = {};
    if (mins < 1) {
        return shell_tr("sh_ago_now");
    }
    if (mins < 60) {
        std::snprintf(buf, sizeof(buf), shell_tr("sh_ago_minutes").c_str(), mins);
    } else if (mins < 60 * 24) {
        std::snprintf(buf, sizeof(buf), shell_tr("sh_ago_hours").c_str(), mins / 60);
    } else if (mins < 60 * 24 * 7) {
        std::snprintf(buf, sizeof(buf), shell_tr("sh_ago_days").c_str(), mins / (60 * 24));
    } else if (mins < 60 * 24 * 30) {
        std::snprintf(buf, sizeof(buf), shell_tr("sh_ago_weeks").c_str(), mins / (60 * 24 * 7));
    } else {
        std::snprintf(buf, sizeof(buf), shell_tr("sh_ago_months").c_str(), mins / (60 * 24 * 30));
    }
    return buf;
}

void open_path(const std::string& utf8_path) {
    const std::wstring w = widen(utf8_path);
    if (!w.empty()) {
        ::ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void open_url(const char* url) {
    if (url != nullptr && *url != '\0') {
        ::ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// Sidebar pages in order. KEYS, not English: the painter translates them, so
// switching languages repaints the sidebar with zero extra work.
constexpr const char* kNavKeys[6] = {"sh_nav_projects", "sh_nav_new", "sh_nav_learn",
                                     "sh_nav_store", "sh_nav_settings", "sh_nav_help"};

// --- subclass for button hover ----------------------------------------------


// --- shell painting & hit-testing -------------------------------------------
// One painter per page plus a shared card/sidebar kit. Layout is computed
// from the client size by small helpers so paint and click handling cannot
// drift apart: hit_test() reuses the same rects the painter drew.


constexpr int kSideW = 200; // sidebar width, logical px

inline int S(const LauncherState* st, int px) { return scale_dpi(st, px); }

void paint_round(HDC dc, int x, int y, int w, int h, int r, COLORREF fill, COLORREF line) {
    RECT rc{x, y, x + w, y + h};
    fill_round(dc, rc, fill, r);
    if (line != fill) {
        frame_round(dc, rc, line, r);
    }
}

void paint_text(HDC dc, const wchar_t* s, int x, int y, int w, int h, HFONT font, COLORREF color,
                UINT fmt = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
    // RTL chokepoint: every hand-drawn string flips alignment here, so no call
    // site needs its own direction branch. (DT_LEFT is 0, so "no LEFT bit"
    // means left — set RIGHT; a RIGHT bit means an explicit right that mirrors
    // back to left. CENTER is directionless and stays.)
    if (g_shell_rtl && (fmt & DT_CENTER) == 0) {
        if ((fmt & DT_RIGHT) != 0) {
            fmt &= static_cast<UINT>(~DT_RIGHT);
        } else {
            fmt |= DT_RIGHT;
        }
        // shell_w_str already shaped the string into visual order; only the
        // box alignment still needs mirroring.
    }
    draw_text_ext(dc, s, x, y, w, h, font, color, fmt);
}

// Translated + shaped text for hand-drawn positions: the single chokepoint
// every painter uses instead of raw literals (see shell_w_str for why native
// controls do NOT come here).
inline void paint_text_u(HDC dc, const std::string& logical_utf8, int x, int y, int w, int h,
                         HFONT font, COLORREF color,
                         UINT fmt = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
    paint_text(dc, shell_w_str(logical_utf8).c_str(), x, y, w, h, font, color, fmt);
}

inline void paint_text_key(HDC dc, const char* key, int x, int y, int w, int h, HFONT font,
                           COLORREF color,
                           UINT fmt = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
    paint_text_u(dc, shell_tr(key), x, y, w, h, font, color, fmt);
}

// Status line (SS_OWNERDRAW STATIC): background + cached logical text.
//
// The status carries MIXED scripts — an Arabic prefix line above an English
// error/path line — and the native STATIC draw scrambles those runs (screenshot:
// the Latin path rendered mirrored). Hand-drawn text goes through the same
// shaping helper as paint_text, so the Latin run inside an Arabic-context line
// stays in order. Multi-line (the failure prefix and its detail are separate
// lines), so no DT_SINGLELINE — word-break wraps a long path instead.
void draw_status(const DRAWITEMSTRUCT* dis, const LauncherState* st) {
    RECT rc = dis->rcItem;
    if (st != nullptr && st->brush_bg != nullptr) {
        ::FillRect(dis->hDC, &rc, st->brush_bg);
    }
    UINT fmt = DT_LEFT | DT_WORDBREAK | DT_NOPREFIX;
    if (g_shell_rtl) {
        // Same "V-center only for single-line" omission as paint_text: this is
        // a small multi-line report, drawn from the top when it overflows.
        fmt |= DT_RIGHT;
    }
    const wchar_t* s = (st != nullptr && !st->status_text.empty())
                           ? st->status_text.c_str() : L"";
    draw_text_ext(dis->hDC, s, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                  st != nullptr ? st->font_small : nullptr,
                  st != nullptr ? st->status_color : theme::error, fmt);
}

// Thumbnail blit, aspect-fill into (x,y,w,h). Missing bitmap = vertical
// gradient block so absent art never breaks layout.
void paint_thumb(HDC dc, const Thumb& th, int x, int y, int w, int h) {
    if (th.bmp == nullptr || th.w <= 0 || th.h <= 0) {
        for (int i = 0; i < h; ++i) {
            const int t = (h > 1) ? (i * 255 / (h - 1)) : 0;
            HBRUSH br = ::CreateSolidBrush(
                RGB(30 + t / 8, 30 + t / 8, 40 + t / 6));
            RECT row{x, y + i, x + w, y + i + 1};
            ::FillRect(dc, &row, br);
            ::DeleteObject(br);
        }
        return;
    }
    HDC mem = ::CreateCompatibleDC(dc);
    HGDIOBJ old = ::SelectObject(mem, th.bmp);
    // Aspect-fill: cover the box, crop the overflow (project cards in the
    // mockups are full-bleed, never letterboxed). Source screenshots carry
    // their own window chrome at the very top; skip the top band so a title
    // bar never leaks into a card.
    const int skip = th.h * 6 / 100;
    const int sh = th.h - skip;
    const double sx = static_cast<double>(w) / th.w;
    const double sy = static_cast<double>(h) / sh;
    const double s = (sx > sy) ? sx : sy;
    const int dw = static_cast<int>(th.w * s + 0.5);
    const int dh = static_cast<int>(sh * s + 0.5);
    ::SetStretchBltMode(dc, COLORONCOLOR);
    // Aspect-fill overflows the target box by design (dw/dh exceed w/h and the
    // blit is centred), so the DC MUST be clipped to the box first — otherwise
    // the spill paints over the card's own text, its border and whatever sits
    // below it (the hero image used to bleed over the "All projects" label and
    // the grid row underneath).
    const int saved_dc = ::SaveDC(dc);
    ::IntersectClipRect(dc, x, y, x + w, y + h);
    ::StretchBlt(dc, x - (dw - w) / 2, y - (dh - h) / 2, dw, dh, mem, 0, skip, th.w, sh, SRCCOPY);
    ::RestoreDC(dc, saved_dc);
    ::SelectObject(mem, old);
    ::DeleteDC(mem);
}

// Sidebar nav mini-icons: drawn geometric primitives (lines/rects/arcs), not
// typed symbols — every UI font rasterises a rectangle, while emoji/symbol
// coverage varies by machine and version. One helper, 14 px box, accent-aware.
void paint_nav_icon(HDC dc, LauncherState* st, ShellPage page, int x, int y, int s,
                    COLORREF col) {
    HPEN pen = ::CreatePen(PS_SOLID, std::max(1, S(st, 2)), col);
    HGDIOBJ op = ::SelectObject(dc, pen);
    HBRUSH old_br = static_cast<HBRUSH>(::SelectObject(dc, ::GetStockObject(NULL_BRUSH)));
    const int x0 = x, y0 = y, x1 = x + s, y1 = y + s;
    switch (page) {
        case ShellPage::Projects: {
            // 2x2 grid of filled squares.
            HBRUSH fill = ::CreateSolidBrush(col);
            const int q = s / 2;
            const int g = std::max(1, S(st, 2));
            RECT r0{x0, y0, x0 + q - g, y0 + q - g};
            RECT r1{x0 + q, y0, x1, y0 + q - g};
            RECT r2{x0, y0 + q, x0 + q - g, y1};
            RECT r3{x0 + q, y0 + q, x1, y1};
            ::FillRect(dc, &r0, fill);
            ::FillRect(dc, &r1, fill);
            ::FillRect(dc, &r2, fill);
            ::FillRect(dc, &r3, fill);
            ::DeleteObject(fill);
            break;
        }
        case ShellPage::NewProject: {
            // Plus: two filled bars.
            HBRUSH fill = ::CreateSolidBrush(col);
            const int t = std::max(2, S(st, 3));
            const int c = s / 2;
            RECT v{x0 + c - t / 2, y0, x0 + c - t / 2 + t, y1};
            RECT h{x0, y0 + c - t / 2, x1, y0 + c - t / 2 + t};
            ::FillRect(dc, &v, fill);
            ::FillRect(dc, &h, fill);
            ::DeleteObject(fill);
            break;
        }
        case ShellPage::Learn: {
            // Open book: cover outline + spine.
            ::RoundRect(dc, x0, y0, x1, y1, S(st, 3), S(st, 3));
            ::MoveToEx(dc, x0 + s / 2, y0, nullptr);
            ::LineTo(dc, x0 + s / 2, y1);
            break;
        }
        case ShellPage::AssetStore: {
            // Shopping bag: body + handle arc.
            ::RoundRect(dc, x0, y0 + s / 4, x1, y1, S(st, 3), S(st, 3));
            ::Arc(dc, x0 + s / 4, y0 - s / 4, x1 - s / 4, y0 + s * 3 / 4, x0 + s / 4,
                  y0 + s / 4, x1 - s / 4, y0 + s / 4);
            break;
        }
        case ShellPage::Settings: {
            // Gear: ring + hub + four spokes.
            ::Ellipse(dc, x0 + 1, y0 + 1, x1 - 1, y1 - 1);
            const int c = s / 2;
            ::MoveToEx(dc, x0 + c, y0 - 1, nullptr);
            ::LineTo(dc, x0 + c, y1 + 1);
            ::MoveToEx(dc, x0 - 1, y0 + c, nullptr);
            ::LineTo(dc, x1 + 1, y0 + c);
            HBRUSH fill = ::CreateSolidBrush(col);
            const int r = std::max(1, S(st, 2));
            RECT hub{x0 + c - r, y0 + c - r, x0 + c + r, y0 + c + r};
            ::FillRect(dc, &hub, fill);
            ::DeleteObject(fill);
            break;
        }
        case ShellPage::Help: {
            // Question mark: text glyph (always present) in the row font size.
            HFONT f = make_font(11, true, st->dpi);
            draw_text_ext(dc, L"?", x0, y0 - S(st, 2), x1 - x0,
                          (y1 - y0) + S(st, 2), f, col,
                          DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            ::DeleteObject(f);
            break;
        }
    }
    ::SelectObject(dc, old_br);
    ::SelectObject(dc, op);
    ::DeleteObject(pen);
}

// Sidebar geometry + paint. Returns item height via out param for hit-testing.
// RTL: the whole bar docks to the RIGHT edge (sx offset); text alignment flips
// in paint_text, so every x here is just shifted by sx.
void paint_sidebar(HDC dc, LauncherState* st, int W, int H) {
    const int sw = S(st, kSideW);
    const int sx = g_shell_rtl ? (W - sw) : 0;
    RECT rc{sx, 0, sx + sw, H};
    HBRUSH bg = ::CreateSolidBrush(RGB(38, 38, 46));
    ::FillRect(dc, &rc, bg);
    ::DeleteObject(bg);
    HPEN sep = ::CreatePen(PS_SOLID, 1, theme::border);
    HGDIOBJ op = ::SelectObject(dc, sep);
    ::MoveToEx(dc, g_shell_rtl ? sx : sw, 0, nullptr);
    ::LineTo(dc, g_shell_rtl ? sx : sw, H);
    ::SelectObject(dc, op);
    ::DeleteObject(sep);

    // Brand: the real logo + Arabic wordmark + tagline. The mark used to be a
    // "\u2699" gear glyph drawn at a fixed LEFT offset in the Amiri face: Amiri
    // has no such glyph (it fell back at wrong metrics and clipped), and in the
    // RTL layout it stayed on the left while the wordmark moved right, so the
    // header read as a broken symbol detached from the name. Now the mark is
    // the brand PNG, docked on the bar's outer side with the text beside it
    // (mirrored for RTL like every other row). No PNG -> text-only header.
    const int logo_s = S(st, 40);
    const bool has_logo = st->logo.bmp != nullptr;
    const int logo_x = g_shell_rtl ? (sx + sw - S(st, 16) - logo_s) : (sx + S(st, 16));
    if (has_logo) {
        paint_logo(dc, st->logo, logo_x, S(st, 14), logo_s, S(st, 10));
    }
    const int text_gap = has_logo ? (logo_s + S(st, 26)) : S(st, 16);
    const int text_x = g_shell_rtl ? (sx + S(st, 12)) : (sx + text_gap);
    const int text_w = sw - text_gap - S(st, 12);
    const std::wstring brand = shell_w_str("\xD8\xB3\xD9\x86\xD8\xAF"); // "سند"
    paint_text(dc, brand.c_str(), text_x, S(st, 10), text_w, S(st, 28), st->font_title,
               theme::text, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    paint_text_key(dc, "sh_tagline", text_x, S(st, 38), text_w, S(st, 16), st->font_small,
                   theme::text_dim, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    for (int i = 0; i < 6; ++i) {
        const int iy = S(st, 84) + i * S(st, 42);
        const bool sel = static_cast<int>(st->page) == i;
        const bool hov = st->hover_nav == i;
        if (sel || hov) {
            RECT ir{sx, iy, sx + sw, iy + S(st, 36)};
            HBRUSH hb = ::CreateSolidBrush(sel ? theme::sel_bg : theme::surface);
            ::FillRect(dc, &ir, hb);
            ::DeleteObject(hb);
            if (sel) {
                // Accent bar on the bar's OUTER edge (mirrors with the dock).
                RECT bar = g_shell_rtl ? RECT{sx + sw - S(st, 3), iy, sx + sw, iy + S(st, 36)}
                                       : RECT{sx, iy, sx + S(st, 3), iy + S(st, 36)};
                HBRUSH ab = ::CreateSolidBrush(theme::accent);
                ::FillRect(dc, &bar, ab);
                ::DeleteObject(ab);
            }
        }
        // Mini-icon at the row's outer side, label beside it (hit rows cover
        // the whole row, so icons move nothing clickable).
        const COLORREF ic = sel ? theme::text : theme::text_dim;
        const int isz = S(st, 15);
        const int icy = iy + (S(st, 36) - isz) / 2;
        const int ixx = g_shell_rtl ? (sx + sw - S(st, 32)) : (sx + S(st, 17));
        paint_nav_icon(dc, st, static_cast<ShellPage>(i), ixx, icy, isz, ic);
        const int lab_x = g_shell_rtl ? (sx + S(st, 6)) : (sx + S(st, 40));
        paint_text_key(dc, kNavKeys[i], lab_x, iy, sw - S(st, 46), S(st, 36), st->font_side,
                       sel ? theme::text : theme::text_dim,
                       DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }

    // Footer: real engine version + renderer, honest and static.
    char ver[64] = {};
    std::snprintf(ver, sizeof(ver), shell_tr("sh_version").c_str(), kEngineVersion);
    paint_text_u(dc, ver, sx + S(st, 16), H - S(st, 52), sw - S(st, 24), S(st, 18),
                 st->font_small, theme::ok, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    paint_text_key(dc, "sh_windows_only", sx + S(st, 16), H - S(st, 32), sw - S(st, 24),
                   S(st, 16), st->font_small, theme::text_dim,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

struct Hit {
    enum class Kind {
        None,
        Nav,            // index = ShellPage
        OpenFile,       // "Open project file" button
        NewPage,        // "+ New project" button -> New project page
        ProjectCard,    // index = filtered recent index
        FeaturedOpen,   // open the featured (first filtered) project
        FeaturedFolder, // reveal it in Explorer
        DimTab,         // index = 0 (All/Both), 1 (2D), 2 (3D)
        DimBtn,         // index = 0 (All/Both), 1 (2D), 2 (3D)
        TemplateCard,   // index = template index
        Create,         // create project from the form
        Browse,         // location browse button
        LearnCard,      // index = learn card (acts per card)
        StoreCard,      // index (all disabled "soon" except browse-docs)
        HelpCard,       // index (docs/links)
        RemoveCard,     // grid card × : drop the entry from the recent list
        SearchClear,    // search-box × : clear the filter
        LangEn,         // settings: English
        LangAr,         // settings: Arabic
        ClearRecent,    // settings: forget recent list
        DocsButton,     // bottom wide buttons
        BugButton,
    };
    Kind kind = Kind::None;
    int index = -1;
};



#endif // _WIN32


// --- shell pages -------------------------------------------------------------
// Filtered recent list shared by paint and hit-test (single source of truth).

// Case-insensitive ".nfproj" suffix (drop target + dialog results). Manual
// fold: avoids pulling <cstring> conventions into hot paths for 7 chars.
inline bool has_nfproj_ext(const std::string& p) {
    if (p.size() < 7) {
        return false;
    }
    static const char kExt[8] = ".nfproj";
    for (int i = 0; i < 7; ++i) {
        unsigned char a = static_cast<unsigned char>(p[p.size() - 7 + static_cast<std::size_t>(i)]);
        const unsigned char b = static_cast<unsigned char>(kExt[i]);
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<unsigned char>(a - 'A' + 'a');
        }
        // kExt is already lowercase; fold only the input side.
        if (a != b) {
            return false;
        }
    }
    return true;
}

std::vector<int> filtered_recent(const LauncherState* st) {
    std::vector<int> out;
    if (st == nullptr || st->recent == nullptr) {
        return out;
    }
    for (std::size_t i = 0; i < st->recent->size(); ++i) {
        if (project_matches_search((*st->recent)[i], st->search)) {
            out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

std::wstring wname_of(const std::string& nfproj_path) {
    const std::string stem = std::filesystem::path(nfproj_path).stem().string();
    return widen(stem);
}

// One project card: thumbnail top, name/path/version/edited below.
void paint_project_card(HDC dc, LauncherState* st, const std::string& path, int x, int y, int w,
                        int h, int thumb_idx, bool hov) {
    paint_round(dc, x, y, w, h, S(st, 10), hov ? theme::surface_hi : theme::surface,
                hov ? theme::accent : theme::border);
    // 58% art, but never taller than (card - the three text rows below it):
    // name (8..28) + path (28..44) must end above the meta row (h-22), which
    // at a 150 px card would otherwise collide with a naive 58%.
    const int img_h = std::min(h * 58 / 100, h - 70);
    const Thumb& th = (thumb_idx >= 0 && thumb_idx < 4) ? st->thumbs[thumb_idx] : st->thumbs[0];
    // Clip the image to the card's top rounded corners by painting the image
    // first and re-framing the border over it.
    paint_thumb(dc, th, x + 1, y + 1, w - 2, img_h);
    // Name/dir travel through the shaping path: project paths can be Arabic
    // (Arabic-first UI), so they go through shell_w_str's shaping.
    paint_text(dc, shell_w_str(std::filesystem::path(path).stem().string()).c_str(),
              x + S(st, 12), y + img_h + S(st, 8), w - S(st, 24), S(st, 20), st->font_body,
              theme::text, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    const std::string dir = std::filesystem::path(path).parent_path().string();
    paint_text(dc, shell_w_str(dir).c_str(), x + S(st, 12), y + img_h + S(st, 28), w - S(st, 24),
              S(st, 16), st->font_small, theme::text_dim, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    char meta[128] = {};
    std::snprintf(meta, sizeof(meta), "v%s", kEngineVersion);
    std::string ago = edited_ago(path);
    std::string line = meta;
    if (!ago.empty()) {
        line += "   ";
        line += ago;
    }
    paint_text(dc, shell_w_str(line).c_str(), x + S(st, 12), y + h - S(st, 22), w - S(st, 24),
              S(st, 16), st->font_small, theme::text_dim, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    frame_round(dc, {x, y, x + w, y + h}, hov ? theme::accent : theme::border, S(st, 10));
}

// Monogram card: gradient + big letter + dark title strip below. Photos stay
// on the Projects page (real game art); template/store art does not exist
// yet, and editor-UI screenshots as card art read as broken UI, so these
// pages get clean drawn cards instead.
// Template card banner: subtle gradient + crisp geometric illustration per
// template index (0=Nature, 1=Platformer, 2=Arena FPS, 3=ThirdPerson,
// 4=Starter Scene, 5=Marine). Previously this drew `wn[0]`, which in RTL was
// the LAST letter of the shaped title rather than an initial, and read as
// random isolated letters floating in empty boxes.
void paint_template_art(HDC dc, LauncherState* st, int x, int y, int w, int h, int tpl_idx,
                        bool dim) {
    for (int i = 0; i < h; ++i) {
        const int t = (h > 1) ? (i * 255 / (h - 1)) : 0;
        HBRUSH br = ::CreateSolidBrush(RGB(28 + t / 12, 32 + t / 10, 44 + t / 7));
        RECT row{x, y + i, x + w, y + i + 1};
        ::FillRect(dc, &row, br);
        ::DeleteObject(br);
    }
    const COLORREF col = dim ? RGB(105, 112, 128) : theme::accent;
    const int cx = x + w / 2;
    const int cy = y + h / 2;
    const int r = std::min(w, h) * 28 / 100;
    HPEN pen = ::CreatePen(PS_SOLID, std::max(2, S(st, 2)), col);
    HGDIOBJ old_pen = ::SelectObject(dc, pen);
    HGDIOBJ old_br = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    switch (tpl_idx) {
        case 0: { // Nature: twin mountain peaks + horizon line
            POINT p1[4] = {{cx - r, cy + r * 2 / 3},
                           {cx - r / 4, cy - r * 2 / 3},
                           {cx + r / 2, cy + r * 2 / 3},
                           {cx - r, cy + r * 2 / 3}};
            POINT p2[4] = {{cx - r / 6, cy + r * 2 / 3},
                           {cx + r / 2, cy - r / 3},
                           {cx + r, cy + r * 2 / 3},
                           {cx - r / 6, cy + r * 2 / 3}};
            ::Polyline(dc, p1, 4);
            ::Polyline(dc, p2, 4);
            break;
        }
        case 1: { // Platformer: three ascending platform blocks
            ::RoundRect(dc, cx - r, cy + r / 3, cx - r / 4, cy + r * 2 / 3, S(st, 3), S(st, 3));
            ::RoundRect(dc, cx - r / 3, cy - r / 6, cx + r / 3, cy + r / 6, S(st, 3), S(st, 3));
            ::RoundRect(dc, cx + r / 4, cy - r * 2 / 3, cx + r, cy - r / 3, S(st, 3), S(st, 3));
            break;
        }
        case 2: { // FPS Arena: crosshair reticle
            ::Ellipse(dc, cx - r * 2 / 3, cy - r * 2 / 3, cx + r * 2 / 3, cy + r * 2 / 3);
            ::MoveToEx(dc, cx - r, cy, nullptr);
            ::LineTo(dc, cx - r / 4, cy);
            ::MoveToEx(dc, cx + r / 4, cy, nullptr);
            ::LineTo(dc, cx + r, cy);
            ::MoveToEx(dc, cx, cy - r, nullptr);
            ::LineTo(dc, cx, cy - r / 4);
            ::MoveToEx(dc, cx, cy + r / 4, nullptr);
            ::LineTo(dc, cx, cy + r);
            break;
        }
        case 3: { // Third-Person: character silhouette (head + shoulders + orbit arc)
            ::Ellipse(dc, cx - r / 3, cy - r * 3 / 4, cx + r / 3, cy - r / 12);
            ::RoundRect(dc, cx - r * 2 / 3, cy, cx + r * 2 / 3, cy + r * 3 / 4, S(st, 6), S(st, 6));
            break;
        }
        case 4: { // Starter Scene: isometric 3D cube
            const int dx = r * 4 / 5;
            const int dy = r * 2 / 5;
            POINT top[5] = {{cx, cy - r}, {cx + dx, cy - r + dy}, {cx, cy - r + dy * 2},
                            {cx - dx, cy - r + dy}, {cx, cy - r}};
            ::Polyline(dc, top, 5);
            ::MoveToEx(dc, cx - dx, cy - r + dy, nullptr);
            ::LineTo(dc, cx - dx, cy + r - dy);
            ::LineTo(dc, cx, cy + r);
            ::LineTo(dc, cx + dx, cy + r - dy);
            ::LineTo(dc, cx + dx, cy - r + dy);
            ::MoveToEx(dc, cx, cy - r + dy * 2, nullptr);
            ::LineTo(dc, cx, cy + r);
            break;
        }
        default: { // Marine: two wave arcs
            for (int row = -1; row <= 1; row += 2) {
                const int wy = cy + row * (r / 3);
                ::MoveToEx(dc, cx - r, wy, nullptr);
                ::LineTo(dc, cx - r / 2, wy - r / 4);
                ::LineTo(dc, cx, wy);
                ::LineTo(dc, cx + r / 2, wy + r / 4);
                ::LineTo(dc, cx + r, wy);
            }
            break;
        }
    }
    ::SelectObject(dc, old_br);
    ::SelectObject(dc, old_pen);
    ::DeleteObject(pen);
}
void paint_shell_btn(HDC dc, LauncherState* st, int x, int y, int w, int h, const wchar_t* label,
                     bool primary, bool hov) {
    BtnRole role = primary ? BtnRole::primary : BtnRole::secondary;
    RECT rc{x, y, x + w, y + h};
    // Reuse the owner-draw look: replicate draw_button's essentials inline.
    COLORREF fill = primary ? (hov ? theme::accent_hi : theme::accent)
                            : (hov ? theme::surface_hi : theme::surface);
    COLORREF line = primary ? fill : (hov ? theme::accent : theme::border);
    COLORREF label_col = primary ? theme::on_accent : (hov ? theme::accent : theme::text);
    fill_round(dc, rc, fill, S(st, 6));
    if (!primary) {
        frame_round(dc, rc, line, S(st, 6));
    }
    paint_text(dc, label, x, y, w, h, st->font_btn, label_col, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    (void)role;
}

// Small × action (search-box clear, grid-card remove): quiet until hovered,
// error-red when armed. One painter for both so the affordance reads the same.
void paint_x_btn(HDC dc, LauncherState* st, const RECT& r, bool hov) {
    fill_round(dc, r, hov ? RGB(90, 40, 44) : theme::surface, S(st, 10));
    if (hov) {
        frame_round(dc, r, theme::error, S(st, 10));
    } else {
        frame_round(dc, r, theme::border, S(st, 10));
    }
    paint_text(dc, L"\u00D7", r.left, r.top, r.right - r.left, r.bottom - r.top, st->font_body,
               hov ? theme::error : theme::text_dim, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

// × rect for a grid card: visual outer-top corner (right in LTR, left in RTL),
// computed from the already-mirrored cell so paint and hit-test agree.
inline void card_x_rect(const LauncherState* st, const RECT& cell, RECT& out) {
    const int s = S(st, 24);
    const int rx = g_shell_rtl ? (cell.left + S(st, 6)) : (cell.right - S(st, 6) - s);
    out = {rx, cell.top + S(st, 6), rx + s, cell.top + S(st, 6) + s};
}

// Disabled card overlay ribbon ("soon").
void paint_soon(HDC dc, LauncherState* st, int x, int y, int w, int h) {
    const int bh = S(st, 22);
    const int by = y + (h - bh) / 2;
    paint_round(dc, x, by, w, bh, S(st, 6), theme::bg, theme::border);
    paint_text_key(dc, "sh_soon", x + S(st, 4), by, w - S(st, 8), bh, st->font_small,
                   theme::text_dim, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

// Dimension badge on template cards (2D, 3D, or 2D / 3D).
void paint_dim_badge(HDC dc, LauncherState* st, int x, int y, int tpl_idx) {
    const int bh = S(st, 20);
    const int bw = S(st, (tpl_idx == 4) ? 58 : 42);
    COLORREF bg_col = RGB(24, 32, 48);
    COLORREF border_col = RGB(59, 130, 246);
    COLORREF text_col = RGB(96, 165, 250);
    const wchar_t* label = L"3D";

    if (tpl_idx == 1) { // Platformer2D
        bg_col = RGB(16, 44, 36);
        border_col = RGB(16, 185, 129);
        text_col = RGB(52, 211, 153);
        label = L"2D";
    } else if (tpl_idx == 4) { // Default / Starter (2D / 3D Hybrid)
        bg_col = RGB(38, 20, 68);
        border_col = RGB(139, 92, 246);
        text_col = RGB(192, 132, 252);
        label = L"2D / 3D";
    }

    paint_round(dc, x, y, bw, bh, S(st, 5), bg_col, border_col);
    HFONT old_font = static_cast<HFONT>(::SelectObject(dc, st->font_small));
    COLORREF old_txt = ::SetTextColor(dc, text_col);
    const int old_bk = ::SetBkMode(dc, TRANSPARENT);
    RECT rc{x, y, x + bw, y + bh};
    ::DrawTextW(dc, label, -1, &rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    ::SetBkMode(dc, old_bk);
    ::SetTextColor(dc, old_txt);
    ::SelectObject(dc, old_font);
}

// Generic info card: optional monogram glyph, title, two dim lines, optional
// action. Returns nothing; clicks are resolved by hit_test with the same math.
// RTL: glyph docks right, text column mirrors, the "soon" ribbon docks left.
// Rhythm is deliberately airy (12 px glyph gap, 6 px title-to-line): Arabic
// marks overflow tight boxes and the glyph used to kiss the title.
void paint_info_card(HDC dc, LauncherState* st, int x, int y, int w, int h, const wchar_t* glyph,
                     const wchar_t* title, const wchar_t* line1, const wchar_t* line2, bool hov,
                     bool disabled) {
    paint_round(dc, x, y, w, h, S(st, 10), hov && !disabled ? theme::surface_hi : theme::surface,
                hov && !disabled ? theme::accent : theme::border);
    const COLORREF dim = disabled ? RGB(110, 110, 125) : theme::text_dim;
    const COLORREF fg = disabled ? RGB(170, 170, 185) : theme::text;
    // Single-line cards (line2 == nullptr — every Learn/Help card today):
    // center the block so ~90 px of dead space doesn't pool at the bottom.
    // Paint-only: the card rect (and its hit area) never moves.
    int yo = 0;
    if (line2 == nullptr) {
        yo = (h - (S(st, 22) + S(st, 6) + S(st, 18))) / 2 - S(st, 12);
        if (yo < 0) {
            yo = 0;
        }
    }
    const int gw = S(st, 40);
    const int gx = g_shell_rtl ? (x + w - S(st, 16) - gw) : (x + S(st, 16));
    if (glyph != nullptr && *glyph != L'\0') {
        // Symbol face (Segoe UI), NOT the Amiri display face: these glyphs are
        // emoji/symbols (book, compass, envelope) that Amiri does not carry —
        // drawing them in Amiri falls back at wrong metrics and clips.
        paint_text(dc, glyph, gx, y + yo + S(st, 16), gw, S(st, 44), st->font_sym, fg,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    const int soon_pad = disabled ? S(st, 62) : 0;
    const int tx = g_shell_rtl ? (x + S(st, 16) + soon_pad) : (x + S(st, 68));
    const int tw = w - S(st, 84) - soon_pad;
    paint_text(dc, title, tx, y + yo + S(st, 12), tw, S(st, 22), st->font_body, fg,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    if (line2 == nullptr) {
        paint_text(dc, line1, tx, y + yo + S(st, 40), tw, S(st, 38), st->font_small, dim,
                  DT_LEFT | DT_WORDBREAK);
    } else {
        paint_text(dc, line1, tx, y + yo + S(st, 40), tw, S(st, 18), st->font_small, dim,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        paint_text(dc, line2, tx, y + S(st, 62), tw, S(st, 18), st->font_small, dim,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    if (disabled) {
        paint_soon(dc, st, g_shell_rtl ? (x + S(st, 10)) : (x + w - S(st, 68)), y, S(st, 58), h);
    }
    frame_round(dc, {x, y, x + w, y + h}, hov && !disabled ? theme::accent : theme::border,
                S(st, 10));
}


// Page content origin (inside the client area, next to the sidebar). RTL: the
// sidebar docks right, so content starts at the left margin; width is the same
// either way (window minus sidebar minus both margins).
inline int content_x(const LauncherState* st) {
    return g_shell_rtl ? S(st, 32) : S(st, kSideW + 32);
}
inline int content_w(const LauncherState* st, int W) {
    return W - S(st, kSideW) - S(st, 64);
}

// Layout helpers ALWAYS compute in LTR coordinates, then mirror_rect() flips
// them when RTL. content_x(st) is the PAINT origin (mode-dependent); a layout
// that starts from it and mirrors would double-apply the flip.
inline int content_x_ltr(const LauncherState* st) {
    return S(st, kSideW + 32);
}

// Mirrors a content-area rect computed in LTR coordinates into RTL coordinates
// (same span, flipped). Layout helpers compute LTR then mirror at the end, so
// paint and hit-test share one formula and can never drift apart.
inline void mirror_rect(const LauncherState* st, int W, RECT& r) {
    if (!g_shell_rtl) {
        return;
    }
    const int span = S(st, kSideW + 32) + content_w(st, W) + S(st, 32);
    const int l = r.left;
    r.left = span - r.right;
    r.right = span - l;
}

// Hairline rule under the page head: structures the page (head vs cards) and
// guarantees visible separation where a big title meets the sidebar side.
void paint_head_rule(HDC dc, LauncherState* st, int W) {
    const int x = content_x(st);
    const int y = S(st, 96);
    HPEN pen = ::CreatePen(PS_SOLID, 1, theme::border);
    HGDIOBJ op = ::SelectObject(dc, pen);
    ::MoveToEx(dc, x, y, nullptr);
    ::LineTo(dc, x + content_w(st, W), y);
    ::SelectObject(dc, op);
    ::DeleteObject(pen);
}

void paint_page_head(HDC dc, LauncherState* st, int W, const char* title_key, const char* sub_key) {
    // Full content width: LTR left-aligns from the sidebar side, RTL
    // right-aligns toward the mirrored margin — one box, no W-dependent math.
    // Generous vertical rhythm (title 24/36, sub 64/20): 26 pt Arabic glyphs
    // with marks overflow tight boxes and visually merge with the subtitle.
    const int x = content_x(st);
    const int cw = content_w(st, W);
    const int title_y = (sub_key != nullptr) ? S(st, 16) : S(st, 22);
    paint_text_key(dc, title_key, x, title_y, cw, S(st, 36), st->font_big, theme::text,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    if (sub_key != nullptr) {
        paint_text_key(dc, sub_key, x, S(st, 68), cw, S(st, 20), st->font_small,
                       theme::text_dim, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    paint_head_rule(dc, st, W); // every page head, one place
}

// --- projects page ---------------------------------------------------------
struct ProjectsLayout {
    RECT search_edit{0, 0, 0, 0};
    RECT search_x{0, 0, 0, 0}; // clear-filter × beside the search box
    RECT btn_open{0, 0, 0, 0};
    RECT btn_new{0, 0, 0, 0};
    RECT featured{0, 0, 0, 0};
    RECT feat_open{0, 0, 0, 0};
    RECT feat_folder{0, 0, 0, 0};
    RECT empty_new{0, 0, 0, 0}; // empty-library CTA buttons (no recents at all)
    RECT empty_open{0, 0, 0, 0};
    RECT grid[8];
    int grid_count = 0;
};

ProjectsLayout layout_projects(const LauncherState* st, int W) {
    ProjectsLayout L{};
    const int x = content_x_ltr(st);
    const int cw = content_w(st, W);
    const int btn_w = S(st, 150), btn_h = S(st, 32);
    L.btn_new = {x + cw - btn_w, S(st, 26), x + cw, S(st, 26) + btn_h};
    L.btn_open = {x + cw - btn_w * 2 - S(st, 10), S(st, 26), x + cw - btn_w - S(st, 10),
                  S(st, 26) + btn_h};
    L.search_edit = {x + std::max(S(st, 200), cw - S(st, 660)), S(st, 26),
                     x + cw - btn_w * 2 - S(st, 20), S(st, 26) + btn_h};
    // Clear-filter × : the edit yields its button-side 34 px, the × lives in
    // the freed strip (a child EDIT always paints over hand-drawn art, so the
    // × cannot sit inside the box itself). Mirrored with the edit below.
    L.search_x = {L.search_edit.right - S(st, 32), L.search_edit.top,
                  L.search_edit.right - S(st, 2), L.search_edit.bottom};
    L.search_edit.right -= S(st, 34);
    const int fy = S(st, 108), fh = S(st, 200);
    L.featured = {x, fy, x + cw, fy + fh};
    const int fw = S(st, 150);
    L.feat_open = {x + cw - fw * 2 - S(st, 42), fy + fh - S(st, 48), x + cw - fw - S(st, 42),
                   fy + fh - S(st, 48) + S(st, 32)};
    L.feat_folder = {x + cw - fw - S(st, 32), fy + fh - S(st, 48), x + cw - S(st, 32),
                     fy + fh - S(st, 48) + S(st, 32)};
    // Grid: 4 columns, card 16:10-ish. Rows beyond 2 are clipped (recent caps
    // at 8, and search usually narrows it). Cell rects come from grid_cell()
    // below so paint and hit-testing share one formula.
    L.grid_count = 0;
    // Empty-library CTA pair (only painted/hit when filt is empty AND no query
    // is typed — a query with no hits keeps sh_no_match instead). Centered in
    // the content column, mirrored with everything else.
    {
        const int bw = S(st, 220), bh = S(st, 36), gap = S(st, 12);
        const int bx = x + (cw - (bw * 2 + gap)) / 2;
        const int by = S(st, 250);
        L.empty_new = {bx, by, bx + bw, by + bh};
        L.empty_open = {bx + bw + gap, by, bx + bw * 2 + gap, by + bh};
        mirror_rect(st, W, L.empty_new);
        mirror_rect(st, W, L.empty_open);
    }
    // RTL mirror: computed LTR above, flipped here — hit_test consumes the same
    // layout, so clicks follow the mirrored paint exactly.
    mirror_rect(st, W, L.btn_new);
    mirror_rect(st, W, L.btn_open);
    mirror_rect(st, W, L.search_edit);
    mirror_rect(st, W, L.search_x);
    mirror_rect(st, W, L.featured);
    mirror_rect(st, W, L.feat_open);
    mirror_rect(st, W, L.feat_folder);
    return L;
}

// Grid cell rect shared by paint and hit-test (single source of truth).
// Computed LTR, then mirror_rect() flips BOTH the origin and the column order
// (mirroring columns without moving the origin left the whole grid 200 px too
// far right, sliding under the RTL sidebar — one transform must do both).
// Columns are responsive: narrow windows drop to fewer, wider cards instead
// of squeezing four unreadable slivers (paint and hit-test share this, so the
// count can never disagree between them).
inline int grid_cols(const LauncherState* st, int W, int three_col) {
    const int cw = content_w(st, W);
    if (three_col != 0) {
        return (cw > S(st, 1100)) ? 3 : 2;
    }
    if (cw > S(st, 1500)) {
        return 4;
    }
    return (cw > S(st, 1050)) ? 3 : 2;
}

inline void grid_cell(const LauncherState* st, int W, int featured_bottom, int i, RECT& out) {
    const int x = content_x_ltr(st);
    const int cw = content_w(st, W);
    const int cols = grid_cols(st, W, 0);
    const int gap = S(st, 16);
    const int gw = (cw - gap * (cols - 1)) / cols;
    const int gh = S(st, 150);
    const int gy = featured_bottom + S(st, 56);
    const int c = i % cols, r = i / cols;
    out.left = x + c * (gw + gap);
    out.top = gy + r * (gh + gap);
    out.right = out.left + gw;
    out.bottom = out.top + gh;
    mirror_rect(st, W, out);
}

// A grid row must clear the status line at the bottom of the window. Rows
// that do not fit are not part of the page at all — paint AND hit-test skip
// them through this one predicate, so a click can never reach an unpainted
// card and no row is ever drawn half-over the status text.
inline bool grid_cell_visible(const LauncherState* st, int H, const RECT& cell) {
    return cell.bottom <= H - S(st, 44);
}

void paint_projects(HDC dc, LauncherState* st, int W, int H, const ProjectsLayout& L,
                    const std::vector<int>& filt) {
    paint_page_head(dc, st, W, "sh_projects_title", "sh_projects_sub");
    paint_shell_btn(dc, st, L.btn_open.left, L.btn_open.top, L.btn_open.right - L.btn_open.left,
                    L.btn_open.bottom - L.btn_open.top, shell_w_key("sh_btn_open_file").c_str(),
                    false, st->hover_btn == 1);
    paint_shell_btn(dc, st, L.btn_new.left, L.btn_new.top, L.btn_new.right - L.btn_new.left,
                    L.btn_new.bottom - L.btn_new.top, shell_w_key("sh_btn_new_project").c_str(),
                    true, st->hover_btn == 2);
    // The × beside the search box (see layout_projects): same affordance as
    // the card × below; hidden until hovered is pointless here — the box shows
    // it whenever a query is typed.
    if (!st->search.empty()) {
        paint_x_btn(dc, st, L.search_x, st->hover_btn == 5);
    }
    if (!filt.empty()) {
        const std::string& path = (*st->recent)[static_cast<std::size_t>(filt[0])];
        const RECT& f = L.featured;
        const int fw = f.right - f.left, fh = f.bottom - f.top;
        paint_round(dc, f.left, f.top, fw, fh, S(st, 12), theme::surface, theme::border);
        // Cover the box, but cap the width at the shot's own 16:9 so a wide
        // (or maximised) window does not zoom the hero into a cropped sliver.
        const int img_w = std::min(fw * 52 / 100, (fh - 2) * 16 / 9);
        // RTL: hero art docks right, text column sits left.
        const int img_x = g_shell_rtl ? (f.right - 1 - img_w) : (f.left + 1);
        paint_thumb(dc, st->thumbs[0], img_x, f.top + 1, img_w, fh - 2);
        const int ix = g_shell_rtl ? (f.left + S(st, 28)) : (f.left + img_w + S(st, 28));
        paint_text(dc, shell_w_str(std::filesystem::path(path).stem().string()).c_str(), ix,
                   f.top + S(st, 22), fw - img_w - S(st, 60), S(st, 30), st->font_title,
                   theme::text, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        paint_text(dc, shell_w_str(path).c_str(), ix, f.top + S(st, 56), fw - img_w - S(st, 60),
                   S(st, 18), st->font_small, theme::text_dim,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        char meta[160] = {};
        std::snprintf(meta, sizeof(meta), shell_tr("sh_hero_engine").c_str(), kEngineVersion,
                      kRendererName);
        std::string ago = edited_ago(path);
        std::string line = meta;
        if (!ago.empty()) {
            char edited[128] = {};
            std::snprintf(edited, sizeof(edited), shell_tr("sh_hero_edited").c_str(), ago.c_str());
            line += "   ";
            line += edited;
        }
        paint_text(dc, shell_w_str(line).c_str(), ix, f.top + S(st, 80), fw - img_w - S(st, 60),
                   S(st, 18), st->font_small, theme::text_dim,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        // Play glyph points outward: right in LTR, left in RTL.
        const std::wstring open_label =
            (g_shell_rtl ? L"\u25C0 " : L"\u25B6 ") + shell_w_key("sh_btn_open_project");
        paint_shell_btn(dc, st, L.feat_open.left, L.feat_open.top,
                        L.feat_open.right - L.feat_open.left, L.feat_open.bottom - L.feat_open.top,
                        open_label.c_str(), true, st->hover_btn == 3);
        paint_shell_btn(dc, st, L.feat_folder.left, L.feat_folder.top,
                        L.feat_folder.right - L.feat_folder.left,
                        L.feat_folder.bottom - L.feat_folder.top,
                        shell_w_key("sh_btn_show_folder").c_str(), false, st->hover_btn == 4);
        frame_round(dc, f, theme::border, S(st, 12));
    }
    char count[64] = {};
    std::snprintf(count, sizeof(count), shell_tr("sh_all_projects").c_str(), filt.size());
    // Full content width: LTR starts at the sidebar side, RTL hugs the far margin.
    paint_text(dc, shell_w_str(count).c_str(), content_x(st), L.featured.bottom + S(st, 18),
               content_w(st, W), S(st, 20), st->font_body, theme::text,
               DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    int shown = 0;
    int grid_bottom = L.featured.bottom;
    for (std::size_t k = 0; k < filt.size() && shown < 8; ++k, ++shown) {
        RECT cell{0, 0, 0, 0};
        grid_cell(st, W, L.featured.bottom, shown, cell);
        if (!grid_cell_visible(st, H, cell)) {
            break; // this row would run into the status line
        }
        const std::string& path =
            (*st->recent)[static_cast<std::size_t>(filt[k])];
        paint_project_card(dc, st, path, cell.left, cell.top, cell.right - cell.left,
                           cell.bottom - cell.top, filt[k] % 4, st->hover_card == filt[k]);
        grid_bottom = cell.bottom;
        // Forget-entry × (top outer corner): removes the row from the recent
        // list (ClearRecent in Settings wipes all; this drops one). Hover-only
        // like premium launchers: the card stays clean until aimed at.
        if (st->hover_btn == 6 && st->hover_card == filt[k]) {
            RECT xr{0, 0, 0, 0};
            card_x_rect(st, cell, xr);
            paint_x_btn(dc, st, xr, true);
        }
    }
    // Rows clipped by the status line (narrow windows / long lists) announce
    // themselves instead of silently hiding projects. Sits under the last
    // visible row, above the status zone (which grid_cell_visible protects).
    const int hidden = static_cast<int>(filt.size()) - shown;
    if (hidden > 0 && grid_bottom + S(st, 28) <= H - S(st, 44)) {
        char more[64] = {};
        std::snprintf(more, sizeof(more), shell_tr("sh_grid_more").c_str(), hidden);
        paint_text(dc, shell_w_str(more).c_str(), content_x(st), grid_bottom + S(st, 10),
                   content_w(st, W), S(st, 18), st->font_small, theme::text_dim,
                   DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    if (filt.empty()) {
        if (st->search.empty()) {
            // Empty library: a real CTA, not the no-match line (which is for
            // an actual query). Buttons reuse the New/Open hit kinds, so hover
            // and click plumbing comes for free. Stacked above the buttons
            // (whose y comes from the layout, S(250)).
            paint_text_key(dc, "sh_empty_title", content_x(st), S(st, 170), content_w(st, W),
                           S(st, 30), st->font_title, theme::text,
                           DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            paint_text_key(dc, "sh_empty_hint", content_x(st), S(st, 204), content_w(st, W),
                           S(st, 18), st->font_small, theme::text_dim,
                           DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            paint_shell_btn(dc, st, L.empty_new.left, L.empty_new.top,
                            L.empty_new.right - L.empty_new.left,
                            L.empty_new.bottom - L.empty_new.top,
                            shell_w_key("sh_btn_new_project").c_str(), true, st->hover_btn == 2);
            paint_shell_btn(dc, st, L.empty_open.left, L.empty_open.top,
                            L.empty_open.right - L.empty_open.left,
                            L.empty_open.bottom - L.empty_open.top,
                            shell_w_key("sh_btn_open_file").c_str(), false, st->hover_btn == 1);
        } else {
            paint_text_key(dc, "sh_no_match", content_x(st), L.featured.bottom + S(st, 44),
                           content_w(st, W), S(st, 18), st->font_small, theme::text_dim,
                           DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
    }
}

// --- new project page -------------------------------------------------------
struct NewLayout {
    RECT dim_tabs[3]; // 0 = All (2D+3D), 1 = 2D, 2 = 3D
    RECT cards[6];
    RECT dim_btns[3]; // In project setup form: 0 = All/Hybrid, 1 = 2D, 2 = 3D
    RECT name_edit{0, 0, 0, 0};
    RECT loc_edit{0, 0, 0, 0};
    RECT browse{0, 0, 0, 0};
    RECT create{0, 0, 0, 0};
    RECT cancel{0, 0, 0, 0};
    int label_w = 0; // label column width (shrinks on a narrow window)
};

NewLayout layout_new(const LauncherState* st, int W) {
    NewLayout L{};
    const int x = content_x_ltr(st);
    const int cw = content_w(st, W);

    // Dimension filter tabs at the top (All / 2D / 3D)
    // Placed on the opposite side of the page title (like Projects buttons on the header row)
    const int tab_h = S(st, 32);
    const int tab_y = S(st, 26);
    const int tw0 = S(st, 190);
    const int tw1 = S(st, 140);
    const int tw2 = S(st, 140);
    const int tgap = S(st, 8);
    const int total_tw = tw0 + tw1 + tw2 + tgap * 2;
    const int tab_start_x = x + cw - total_tw;
    L.dim_tabs[0] = {tab_start_x, tab_y, tab_start_x + tw0, tab_y + tab_h};
    L.dim_tabs[1] = {tab_start_x + tw0 + tgap, tab_y, tab_start_x + tw0 + tgap + tw1, tab_y + tab_h};
    L.dim_tabs[2] = {tab_start_x + tw0 + tgap + tw1 + tgap, tab_y, tab_start_x + total_tw, tab_y + tab_h};
    mirror_rect(st, W, L.dim_tabs[0]);
    mirror_rect(st, W, L.dim_tabs[1]);
    mirror_rect(st, W, L.dim_tabs[2]);

    const int cols = grid_cols(st, W, 1), gap = S(st, 16);
    const int gw = (cw - gap * (cols - 1)) / cols;
    const int gh = S(st, 150);
    const int gy = S(st, 108);
    int rows = 1;
    for (int i = 0; i < 6; ++i) {
        const int c = i % cols, r = i / cols;
        L.cards[i] = {x + c * (gw + gap), gy + r * (gh + gap), x + c * (gw + gap) + gw,
                      gy + r * (gh + gap) + gh};
        rows = std::max(rows, r + 1);
        mirror_rect(st, W, L.cards[i]);
    }
    const int fy = gy + rows * (gh + gap) + S(st, 24);
    int label_w = S(st, 130);
    int bw = S(st, 110);
    const int avail = cw - label_w - S(st, 10) - bw - S(st, 10);
    if (avail < S(st, 120)) {
        label_w = std::max(0, cw - bw - S(st, 10) - S(st, 120));
        bw = std::max(S(st, 60), bw);
    }
    const int field_x = x + label_w + S(st, 10);
    L.name_edit = {field_x, fy, x + cw, fy + S(st, 28)};
    L.loc_edit = {field_x, fy + S(st, 38), x + cw - bw - S(st, 10), fy + S(st, 66)};
    if (L.loc_edit.right <= L.loc_edit.left) L.loc_edit.right = L.loc_edit.left + S(st, 120);
    if (L.name_edit.right <= L.name_edit.left) L.name_edit.right = L.name_edit.left + S(st, 120);
    L.browse = {x + cw - bw, fy + S(st, 38), x + cw, fy + S(st, 66)};

    // Dimension toggle pills in project setup form
    const int dim_fy = fy + S(st, 76);
    const int db_w = S(st, 140);
    L.dim_btns[1] = {field_x, dim_fy, field_x + db_w, dim_fy + S(st, 26)};
    L.dim_btns[2] = {field_x + db_w + S(st, 8), dim_fy, field_x + db_w * 2 + S(st, 8), dim_fy + S(st, 26)};
    L.dim_btns[0] = {field_x + (db_w + S(st, 8)) * 2, dim_fy, field_x + (db_w + S(st, 8)) * 2 + db_w, dim_fy + S(st, 26)};
    mirror_rect(st, W, L.dim_btns[0]);
    mirror_rect(st, W, L.dim_btns[1]);
    mirror_rect(st, W, L.dim_btns[2]);

    L.create = {x + cw - S(st, 300), fy + S(st, 148), x + cw - S(st, 150), fy + S(st, 180)};
    L.cancel = {x + cw - S(st, 140), fy + S(st, 148), x + cw, fy + S(st, 180)};
    L.label_w = label_w;
    mirror_rect(st, W, L.name_edit);
    mirror_rect(st, W, L.loc_edit);
    mirror_rect(st, W, L.browse);
    mirror_rect(st, W, L.create);
    mirror_rect(st, W, L.cancel);
    return L;
}

void paint_new(HDC dc, LauncherState* st, int W, const NewLayout& L) {
    paint_page_head(dc, st, W, "sh_new_title", nullptr);

    // Dimension filter tabs at the top (All / 2D / 3D)
    static const char* const kDimTabKeys[3] = {"sh_dim_all", "sh_dim_2d", "sh_dim_3d"};
    for (int t = 0; t < 3; ++t) {
        const RECT& r = L.dim_tabs[t];
        const int rw = r.right - r.left, rh = r.bottom - r.top;
        const bool active = (st->project_dim == t);
        const bool hov = (st->hover_btn == (20 + t));
        paint_round(dc, r.left, r.top, rw, rh, S(st, 6),
                    active ? theme::accent : (hov ? theme::surface_hi : theme::surface),
                    active ? theme::accent : (hov ? theme::accent : theme::border));
        paint_text_key(dc, kDimTabKeys[t], r.left + S(st, 4), r.top, rw - S(st, 8), rh,
                       st->font_btn, active ? theme::on_accent : (hov ? theme::accent : theme::text),
                       DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    for (int i = 0; i < 6; ++i) {
        const RECT& c = L.cards[i];
        const int cw = c.right - c.left, ch = c.bottom - c.top;
        const bool en = kTemplates[i].enabled;
        const bool hov = st->hover_card == i && en;
        paint_round(dc, c.left, c.top, cw, ch, S(st, 10), hov ? theme::surface_hi : theme::surface,
                    hov ? theme::accent : theme::border);
        const int strip = S(st, 52);
        const std::wstring wn = shell_w_key(kTemplates[i].name_key);
        const std::wstring wd = shell_w_key(kTemplates[i].desc_key);
        paint_template_art(dc, st, c.left + 1, c.top + 1, cw - 2, ch - strip - 1, i, !en);
        HBRUSH sb = ::CreateSolidBrush(hov ? theme::surface_hi : theme::surface);
        RECT sr{c.left + 1, c.bottom - strip, c.right - 1, c.bottom - 1};
        ::FillRect(dc, &sr, sb);
        ::DeleteObject(sb);
        paint_text(dc, wn.c_str(), c.left + S(st, 10), c.bottom - strip, cw - S(st, 20),
                   S(st, 24), st->font_body, en ? theme::text : theme::text_dim,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        paint_text(dc, wd.c_str(), c.left + S(st, 10), c.bottom - strip + S(st, 24),
                   cw - S(st, 20), S(st, 20), st->font_small, theme::text_dim,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (!en) {
            paint_soon(dc, st, g_shell_rtl ? (c.left + S(st, 8)) : (c.right - S(st, 70)),
                       c.top + S(st, 6), S(st, 62), S(st, 24));
        }
        // Dimension badge in top corner
        {
            const int bw = S(st, (i == 4) ? 58 : 42);
            const int bx = g_shell_rtl ? (c.right - bw - S(st, 8)) : (c.left + S(st, 8));
            paint_dim_badge(dc, st, bx, c.top + S(st, 6), i);
        }

        if (st->selected_template == i && en) {
            const COLORREF frame_col = (i == 1) ? RGB(16, 185, 129) : theme::accent;
            frame_round(dc, c, frame_col, S(st, 10), 2);
        } else {
            frame_round(dc, c, theme::border, S(st, 10));
        }
    }
    const int x = content_x(st);
    const int cw = content_w(st, W);
    const int fy = L.name_edit.top;
    paint_text_key(dc, "sh_setup", x, fy - S(st, 30), cw, S(st, 22), st->font_body,
                   theme::text, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    const int lab_w = L.label_w;
    const int lab_x = g_shell_rtl ? (x + cw - lab_w) : x;
    paint_text_key(dc, "sh_name_label", lab_x, L.name_edit.top, lab_w, S(st, 28), st->font_body,
                   theme::text_dim, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    paint_text_key(dc, "sh_loc_label", lab_x, L.loc_edit.top, lab_w, S(st, 28), st->font_body,
                   theme::text_dim, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // Dimension Choice Buttons in setup form
    const int dim_fy = fy + S(st, 76);
    paint_text_key(dc, "sh_dim_label", lab_x, dim_fy, lab_w, S(st, 26), st->font_body,
                   theme::text_dim, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    static const int kDimBtnIdx[3] = {1, 2, 0}; // 2D, 3D, All
    static const char* const kDimBtnKeys[3] = {"sh_dim_opt_2d", "sh_dim_opt_3d", "sh_dim_opt_all"};
    for (int b = 0; b < 3; ++b) {
        const int d_idx = kDimBtnIdx[b];
        const RECT& br = L.dim_btns[d_idx];
        const int bw = br.right - br.left, bh = br.bottom - br.top;
        const bool active = (st->project_dim == d_idx);
        const bool hov = (st->hover_btn == (30 + d_idx));
        COLORREF act_col = (d_idx == 1) ? RGB(16, 185, 129) : ((d_idx == 2) ? RGB(59, 130, 246) : RGB(139, 92, 246));
        paint_round(dc, br.left, br.top, bw, bh, S(st, 6),
                    active ? act_col : (hov ? theme::surface_hi : theme::surface),
                    active ? act_col : (hov ? theme::accent : theme::border));
        paint_text_key(dc, kDimBtnKeys[b], br.left + S(st, 4), br.top, bw - S(st, 8), bh,
                       st->font_small, active ? theme::on_accent : (hov ? theme::accent : theme::text),
                       DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    // Target Renderer (moved down below dimension buttons)
    const int rend_y = dim_fy + S(st, 38);
    paint_text_key(dc, "sh_renderer_label", lab_x, rend_y, lab_w, S(st, 20),
                   st->font_body, theme::text_dim, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    const int value_right = L.name_edit.right;
    const int value_w = (value_right > L.loc_edit.left)
                            ? (value_right - L.loc_edit.left)
                            : S(st, 120);
    paint_text_key(dc, "sh_renderer_value", L.loc_edit.left, rend_y,
                   value_w, S(st, 20), st->font_small, theme::text,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    paint_shell_btn(dc, st, L.browse.left, L.browse.top, L.browse.right - L.browse.left,
                    L.browse.bottom - L.browse.top, shell_w_key("sh_btn_browse").c_str(), false,
                    st->hover_btn == 11);
    paint_shell_btn(dc, st, L.create.left, L.create.top, L.create.right - L.create.left,
                    L.create.bottom - L.create.top, shell_w_key("sh_btn_create").c_str(), true,
                    st->hover_btn == 12);
    paint_shell_btn(dc, st, L.cancel.left, L.cancel.top, L.cancel.right - L.cancel.left,
                    L.cancel.bottom - L.cancel.top, shell_w_key("sh_btn_cancel").c_str(), false,
                    st->hover_btn == 13);
}

// --- learn / store / settings / help pages -----------------------------------
struct LearnCard {
    const wchar_t* glyph;
    const char* title_key;
    const char* line_key;
    bool enabled;
};
constexpr LearnCard kLearn[6] = {
    {L"\U0001F4D6", "sh_learn_academy", "sh_learn_academy_sub", true},
    {L"\U0001F9ED", "sh_learn_script", "sh_learn_script_sub", false},
    {L"\U0001F4E6", "sh_learn_pipeline", "sh_learn_pipeline_sub", true},
    {L"</>", "sh_learn_api", "sh_learn_api_sub", true},
    {L"\u269B", "sh_learn_physics", "sh_learn_physics_sub", true},
    {L"\u2728", "sh_learn_gfx", "sh_learn_gfx_sub", true},
};

constexpr LearnCard kHelp[6] = {
    {L"\U0001F4D6", "sh_help_docs", "sh_help_docs_sub", true},
    {L"\U0001F393", "sh_help_tuts", "sh_help_tuts_sub", true},
    {L"\U0001F4AC", "sh_help_forum", "sh_help_forum_sub", false},
    {L"\U0001F4CB", "sh_help_notes", "sh_help_notes_sub", true},
    {L"\u2709", "sh_help_support", "sh_help_support_sub", false},
    {L"\u2605", "sh_help_feature", "sh_help_feature_sub", false},
};

// Card-grid cell shared by paint AND hit-test (single source of truth, Learn /
// Help / Store all use the same 3-column geometry; gh differs per page and is
// passed in so paint and hit-test cannot disagree about it).
inline void card_grid_cell(const LauncherState* st, int W, int i, int gh_in, RECT& out) {
    const int x = content_x_ltr(st);
    const int cw = content_w(st, W);
    const int cols = grid_cols(st, W, 1), gap = S(st, 16);
    const int gw = (cw - gap * (cols - 1)) / cols;
    const int gy = S(st, 108);
    const int c = i % cols, r = i / cols;
    out.left = x + c * (gw + gap);
    out.top = gy + r * (gh_in + gap);
    out.right = out.left + gw;
    out.bottom = out.top + gh_in;
    // Same one-transform rule as grid_cell(): mirror moves the origin AND the
    // column order together — column-swapping alone slid grids under the RTL
    // sidebar while hit-testing (same helper) still agreed with the paint.
    mirror_rect(st, W, out);
}

void paint_card_grid(HDC dc, LauncherState* st, int W, const LearnCard* cards, int& hov_id) {
    for (int i = 0; i < 6; ++i) {
        RECT cell{0, 0, 0, 0};
        card_grid_cell(st, W, i, S(st, 150), cell);
        paint_info_card(dc, st, cell.left, cell.top, cell.right - cell.left,
                        cell.bottom - cell.top, cards[i].glyph, shell_w_key(cards[i].title_key).c_str(),
                        shell_w_key(cards[i].line_key).c_str(), nullptr, hov_id == i,
                        !cards[i].enabled);
    }
}

void paint_settings(HDC dc, LauncherState* st, int W) {
    paint_page_head(dc, st, W, "sh_settings_title", nullptr);
    const int cw = content_w(st, W);
    const int gw = (cw - S(st, 32)) / 3;
    const int gh = S(st, 170);
    const int gy = S(st, 108);
    // Card rects in LTR order, mirrored for RTL (first card docks right).
    RECT cards[3];
    for (int k = 0; k < 3; ++k) {
        cards[k] = {content_x_ltr(st) + k * (gw + S(st, 16)), gy,
                    content_x_ltr(st) + k * (gw + S(st, 16)) + gw, gy + gh};
        mirror_rect(st, W, cards[k]);
    }
    // General: language radios (REAL — returned in LauncherResult.arabic).
    {
        const RECT& c0 = cards[0];
        const bool hov = st->hover_card == 100;
        paint_round(dc, c0.left, c0.top, gw, gh, S(st, 10), hov ? theme::surface_hi : theme::surface,
                    hov ? theme::accent : theme::border);
        paint_text_key(dc, "sh_general", c0.left + S(st, 16), gy + S(st, 12), gw - S(st, 32),
                       S(st, 22), st->font_body, theme::text,
                       DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        const int ry = gy + S(st, 60);
        const bool en = !st->arabic_ui;
        // Radio circles drawn manually; hit rects resolved in hit_test.
        // RTL: circle docks right, label fills toward the left.
        auto radio = [&](int yy, const char* label_key, bool on) {
            const int cx0 = g_shell_rtl ? (c0.right - S(st, 20) - S(st, 14)) : (c0.left + S(st, 20));
            HBRUSH dot = ::CreateSolidBrush(on ? theme::accent : theme::surface);
            HPEN pen = ::CreatePen(PS_SOLID, 1, on ? theme::accent : theme::text_dim);
            HGDIOBJ ob = ::SelectObject(dc, dot);
            HGDIOBJ ope = ::SelectObject(dc, pen);
            ::Ellipse(dc, cx0, yy, cx0 + S(st, 14), yy + S(st, 14));
            ::SelectObject(dc, ob);
            ::SelectObject(dc, ope);
            ::DeleteObject(dot);
            ::DeleteObject(pen);
            if (on) {
                HBRUSH in = ::CreateSolidBrush(theme::accent);
                ::SelectObject(dc, in);
                ::Ellipse(dc, cx0 + S(st, 4), yy + S(st, 4), cx0 + S(st, 10), yy + S(st, 10));
                ::SelectObject(dc, ob);
                ::DeleteObject(in);
            }
            const int lab_x = g_shell_rtl ? (c0.left + S(st, 16)) : (cx0 + S(st, 22));
            const int lab_w = gw - S(st, 60);
            paint_text_key(dc, label_key, lab_x, yy - S(st, 3), lab_w, S(st, 20), st->font_body,
                           theme::text, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        };
        radio(ry, "sh_radio_en", en);
        radio(ry + S(st, 26), "sh_radio_ar", !en);
        paint_text_key(dc, "sh_applies", c0.left + S(st, 16), gy + gh - S(st, 26), gw - S(st, 32),
                       S(st, 16), st->font_small, theme::text_dim,
                       DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        frame_round(dc, c0, hov ? theme::accent : theme::border, S(st, 10));
    }
    // Project defaults: real paths + clear-recent (REAL).
    {
        const RECT& c1 = cards[1];
        paint_round(dc, c1.left, c1.top, gw, gh, S(st, 10), theme::surface, theme::border);
        paint_text_key(dc, "sh_defaults", c1.left + S(st, 16), gy + S(st, 12), gw - S(st, 32),
                       S(st, 22), st->font_body, theme::text,
                       DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        paint_text_key(dc, "sh_default_loc", c1.left + S(st, 16), gy + S(st, 44), gw - S(st, 32),
                       S(st, 16), st->font_small, theme::text_dim,
                       DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        paint_text_key(dc, "sh_renderer_fixed", c1.left + S(st, 16), gy + S(st, 64),
                       gw - S(st, 32), S(st, 16), st->font_small, theme::text_dim,
                       DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        paint_shell_btn(dc, st, c1.left + S(st, 16), gy + gh - S(st, 44), gw - S(st, 32),
                        S(st, 30), shell_w_key("sh_btn_clear_recent").c_str(), false,
                        st->hover_btn == 21);
        frame_round(dc, c1, theme::border, S(st, 10));
    }
    // Editor & interface: honest subset (grid default mirrors the editor).
    {
        const RECT& c2 = cards[2];
        paint_round(dc, c2.left, c2.top, gw, gh, S(st, 10), theme::surface, theme::border);
        paint_text_key(dc, "sh_editor_iface", c2.left + S(st, 16), gy + S(st, 12), gw - S(st, 32),
                       S(st, 22), st->font_body, theme::text,
                       DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        paint_text_key(dc, "sh_theme_fixed", c2.left + S(st, 16), gy + S(st, 44), gw - S(st, 32),
                       S(st, 16), st->font_small, theme::text_dim,
                       DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        // "·" is U+00B7 (in the atlas range); the Latin version tag stays as-is
        // in both languages — versions never translate.
        char ver[64] = {};
        std::snprintf(ver, sizeof(ver), "Engine %s · %s", kEngineVersion, kRendererName);
        paint_text(dc, shell_w_str(ver).c_str(), c2.left + S(st, 16), gy + S(st, 64),
                   gw - S(st, 32), S(st, 16), st->font_small, theme::text_dim,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        frame_round(dc, c2, theme::border, S(st, 10));
    }
}

void paint_store(HDC dc, LauncherState* st, int W) {
    // Honest placeholder with the store's layout: the backend does not exist,
    // so every product card is visibly disabled rather than faked with prices
    // that do nothing when clicked.
    paint_page_head(dc, st, W, "sh_store_title", "sh_store_sub");
    constexpr const char* cat_keys[6] = {"sh_cat_chars", "sh_cat_textures", "sh_cat_vfx",
                                         "sh_cat_gui", "sh_cat_audio", "sh_cat_plugins"};
    // Monograms in the UI language: Latin initials (3/P/V/G/M/A) read foreign
    // inside the Arabic UI, so each card carries its category's Arabic initial
    // (\u escapes keep the file encoding-robust like the wordmark above).
    constexpr wchar_t glyphs[6] = {L'\u0634', L'\u062E', L'\u0645',
                                    L'\u0648', L'\u0635', L'\u0625'};
    for (int i = 0; i < 6; ++i) {
        RECT cell{0, 0, 0, 0};
        card_grid_cell(st, W, i, S(st, 170), cell);
        const int cx = cell.left, cy = cell.top;
        const int gw = cell.right - cell.left, gh = cell.bottom - cell.top;
        paint_round(dc, cx, cy, gw, gh, S(st, 10), theme::surface, theme::border);
        // Inset by 1 px so the square gradient never bleeds over the card's
        // rounded top corners before the border is re-framed on top.
        paint_template_art(dc, st, cx + 1, cy + 1, gw - 2, gh - S(st, 53), i, true);
        (void)glyphs;
        paint_text_key(dc, cat_keys[i], cx + S(st, 12), cy + gh - S(st, 42), gw - S(st, 24),
                       S(st, 20), st->font_body, theme::text_dim,
                       DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        paint_text_key(dc, "sh_cat_opens", cx + S(st, 12), cy + gh - S(st, 22), gw - S(st, 24),
                       S(st, 14), st->font_small, theme::text_dim,
                       DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        paint_soon(dc, st, g_shell_rtl ? (cx + S(st, 8)) : (cx + gw - S(st, 70)), cy + S(st, 8),
                   S(st, 62), S(st, 24));
        frame_round(dc, cell, theme::border, S(st, 10));
    }
}


// --- shell window ------------------------------------------------------------
// Hit-testing mirrors the painters above: every clickable element is a manual
// rect positioned by the same layout helpers the painters use, so paint and
// clicks cannot drift apart. Text inputs stay real Win32 EDITs.

// Settings radio/clear geometry shared with paint_settings (General card is
// the first of three equal cards; radios live in it, Clear in the second).
// Computed LTR from the same card layout paint_settings uses, then mirrored —
// one formula for both, so clicks follow the mirrored paint exactly.
struct SettingsGeom {
    RECT card0{0, 0, 0, 0};
    RECT clear_btn{0, 0, 0, 0};
    int ry_en = 0;
    int ry_ar = 0;
};
inline void settings_geom(const LauncherState* st, int W, SettingsGeom& g) {
    const int x = content_x_ltr(st);
    const int cw = content_w(st, W);
    const int gw = (cw - S(st, 32)) / 3;
    const int gh = S(st, 170);
    const int gy = S(st, 108);
    g.card0 = {x, gy, x + gw, gy + gh};
    mirror_rect(st, W, g.card0);
    g.ry_en = gy + S(st, 60);
    g.ry_ar = gy + S(st, 86);
    RECT clear{x + gw + S(st, 16) + S(st, 16), gy + gh - S(st, 44),
               x + gw + S(st, 16) + gw - S(st, 16), gy + gh - S(st, 14)};
    mirror_rect(st, W, clear);
    g.clear_btn = clear;
}

inline bool in_rect(int x0, int y0, int x1, int y1, int x, int y) {
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}
inline bool in_rect(const RECT& r, int x, int y) {
    return in_rect(r.left, r.top, r.right, r.bottom, x, y);
}

Hit hit_test(LauncherState* st, int x, int y, int W, int H) {
    Hit h;
    if (st == nullptr) {
        return h;
    }
    const int sw = S(st, kSideW);
    // RTL: the sidebar docks to the right edge (mirrors paint_sidebar).
    const bool in_sidebar = g_shell_rtl ? (x >= W - sw) : (x >= 0 && x < sw);
    if (in_sidebar) {
        for (int i = 0; i < 6; ++i) {
            const int iy = S(st, 84) + i * S(st, 42);
            if (y >= iy && y < iy + S(st, 36)) {
                h.kind = Hit::Kind::Nav;
                h.index = i;
                return h;
            }
        }
        return h;
    }
    if (st->page == ShellPage::Projects) {
        ProjectsLayout L = layout_projects(st, W);
        if (in_rect(L.btn_open, x, y)) {
            h.kind = Hit::Kind::OpenFile;
            return h;
        }
        if (in_rect(L.btn_new, x, y)) {
            h.kind = Hit::Kind::NewPage;
            h.index = 1; // open the New page (index -1 means Cancel→Projects)
            return h;
        }
        // Search × only exists while a query is typed (it is not painted else).
        if (!st->search.empty() && in_rect(L.search_x, x, y)) {
            h.kind = Hit::Kind::SearchClear;
            return h;
        }
        auto filt = filtered_recent(st);
        if (filt.empty()) {
            // Empty library without a query: the CTA pair (same kinds as the
            // header buttons, so hover mapping is shared).
            if (st->search.empty()) {
                if (in_rect(L.empty_new, x, y)) {
                    h.kind = Hit::Kind::NewPage;
                    h.index = 1;
                    return h;
                }
                if (in_rect(L.empty_open, x, y)) {
                    h.kind = Hit::Kind::OpenFile;
                    return h;
                }
            }
            return h;
        }
        {
            if (in_rect(L.feat_open, x, y)) {
                h.kind = Hit::Kind::FeaturedOpen;
                return h;
            }
            if (in_rect(L.feat_folder, x, y)) {
                h.kind = Hit::Kind::FeaturedFolder;
                return h;
            }
            for (std::size_t k = 0; k < filt.size() && k < 8; ++k) {
                RECT cell{0, 0, 0, 0};
                grid_cell(st, W, L.featured.bottom, static_cast<int>(k), cell);
                if (!grid_cell_visible(st, H, cell)) {
                    break; // row is not painted — it cannot be clicked either
                }
                // The forget × sits above the card: test it first so the two
                // never compete for the same pixel.
                RECT xr{0, 0, 0, 0};
                card_x_rect(st, cell, xr);
                if (in_rect(xr, x, y)) {
                    h.kind = Hit::Kind::RemoveCard;
                    h.index = filt[k];
                    return h;
                }
                if (in_rect(cell, x, y)) {
                    h.kind = Hit::Kind::ProjectCard;
                    h.index = filt[k];
                    return h;
                }
            }
        }
        return h;
    }
    if (st->page == ShellPage::NewProject) {
        NewLayout L = layout_new(st, W);
        for (int i = 0; i < 3; ++i) {
            if (in_rect(L.dim_tabs[i], x, y)) {
                h.kind = Hit::Kind::DimTab;
                h.index = i;
                return h;
            }
            if (in_rect(L.dim_btns[i], x, y)) {
                h.kind = Hit::Kind::DimBtn;
                h.index = i;
                return h;
            }
        }
        for (int i = 0; i < 6; ++i) {
            if (kTemplates[i].enabled && in_rect(L.cards[i], x, y)) {
                h.kind = Hit::Kind::TemplateCard;
                h.index = i;
                return h;
            }
        }
        if (in_rect(L.browse, x, y)) {
            h.kind = Hit::Kind::Browse;
            return h;
        }
        if (in_rect(L.create, x, y)) {
            h.kind = Hit::Kind::Create;
            return h;
        }
        if (in_rect(L.cancel, x, y)) {
            h.kind = Hit::Kind::NewPage;
            h.index = -1; // cancel = back to Projects
            return h;
        }
        return h;
    }
    if (st->page == ShellPage::Learn || st->page == ShellPage::Help) {
        // Same cells paint_card_grid draws (card_grid_cell mirrors in RTL).
        for (int i = 0; i < 6; ++i) {
            RECT cell{0, 0, 0, 0};
            card_grid_cell(st, W, i, S(st, 150), cell);
            if (in_rect(cell, x, y)) {
                h.kind = (st->page == ShellPage::Learn) ? Hit::Kind::LearnCard : Hit::Kind::HelpCard;
                h.index = i;
                return h;
            }
        }
        return h;
    }
    if (st->page == ShellPage::Settings) {
        SettingsGeom g{};
        settings_geom(st, W, g);
        if (in_rect(g.card0.left + S(st, 12), g.ry_en - S(st, 6), g.card0.right - S(st, 12),
                    g.ry_en + S(st, 24), x, y)) {
            h.kind = Hit::Kind::LangEn;
            return h;
        }
        if (in_rect(g.card0.left + S(st, 12), g.ry_ar - S(st, 6), g.card0.right - S(st, 12),
                    g.ry_ar + S(st, 24), x, y)) {
            h.kind = Hit::Kind::LangAr;
            return h;
        }
        if (in_rect(g.clear_btn, x, y)) {
            h.kind = Hit::Kind::ClearRecent;
            return h;
        }
        return h;
    }
    return h; // Asset Store: every card is an honest "soon" — nothing clickable
}

// Docs/ tree and ROADMAP.md by the same module walk-up the thumbnails use.
std::wstring docs_dir() {
    wchar_t mod[MAX_PATH * 2] = {};
    DWORD n = ::GetModuleFileNameW(nullptr, mod, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) {
        return {};
    }
    std::filesystem::path dir(mod);
    dir = dir.parent_path();
    for (int i = 0; i < 8; ++i) {
        std::error_code ec;
        if (std::filesystem::is_directory(dir / "Docs", ec)) {
            return (dir / "Docs").wstring();
        }
        if (!dir.has_parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }
    return {};
}

std::wstring roadmap_file() {
    wchar_t mod[MAX_PATH * 2] = {};
    DWORD n = ::GetModuleFileNameW(nullptr, mod, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) {
        return {};
    }
    std::filesystem::path dir(mod);
    dir = dir.parent_path();
    for (int i = 0; i < 8; ++i) {
        std::error_code ec;
        std::filesystem::path cand = dir / "ROADMAP.md";
        if (std::filesystem::exists(cand, ec)) {
            return cand.wstring();
        }
        if (!dir.has_parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }
    return {};
}

void read_search_edit(HWND hwnd, LauncherState* st) {
    wchar_t buf[256] = {};
    ::GetDlgItemTextW(hwnd, kIdSearch, buf, 256);
    st->search = narrow(buf);
}

// Position the child windows (the text inputs and the status line) for the
// current page and client size. Called on every page switch AND every
// WM_SIZE: the window is resizable, so a size change has to drag the EDITs
// and the status line along with the hand-drawn layout.
void layout_children(HWND hwnd, LauncherState* st) {
    RECT rc;
    ::GetClientRect(hwnd, &rc);
    const int W = rc.right - rc.left;
    const int H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) {
        return; // minimised (or sizing during CreateWindow, before children exist)
    }
    HWND search = ::GetDlgItem(hwnd, kIdSearch);
    HWND name = ::GetDlgItem(hwnd, kIdNewName);
    HWND loc = ::GetDlgItem(hwnd, kIdNewDir);
    // Every control may legitimately be absent: WM_SIZE also fires during
    // ShowWindow(SW_MAXIMIZE), before the EDITs are created.
    if (search != nullptr) {
        ::ShowWindow(search, (st->page == ShellPage::Projects) ? SW_SHOW : SW_HIDE);
    }
    const bool is_new = (st->page == ShellPage::NewProject);
    if (name != nullptr) {
        ::ShowWindow(name, is_new ? SW_SHOW : SW_HIDE);
    }
    if (loc != nullptr) {
        ::ShowWindow(loc, is_new ? SW_SHOW : SW_HIDE);
    }
    if (st->page == ShellPage::Projects) {
        ProjectsLayout L = layout_projects(st, W);
        if (search != nullptr) {
            ::MoveWindow(search, L.search_edit.left, L.search_edit.top,
                         L.search_edit.right - L.search_edit.left,
                         L.search_edit.bottom - L.search_edit.top, TRUE);
        }
    } else if (is_new) {
        NewLayout L = layout_new(st, W);
        if (name != nullptr) {
            ::MoveWindow(name, L.name_edit.left, L.name_edit.top,
                         L.name_edit.right - L.name_edit.left,
                         L.name_edit.bottom - L.name_edit.top, TRUE);
        }
        if (loc != nullptr) {
            ::MoveWindow(loc, L.loc_edit.left, L.loc_edit.top, L.loc_edit.right - L.loc_edit.left,
                         L.loc_edit.bottom - L.loc_edit.top, TRUE);
        }
    }
    if (HWND status = ::GetDlgItem(hwnd, kIdStatus)) {
        // Three lines fit here: prefix, error detail, and (under Low
        // integrity) the repair hint. The old 22 px box clipped the second
        // line to an unreadable sliver.
        ::MoveWindow(status, content_x(st), H - S(st, 72), content_w(st, W), S(st, 58), TRUE);
    }
}

// The location EDIT is the source of truth at click time (Browse writes both
// the box and the state, typing writes only the box). Shared by the Create
// button and the Return key in either New-page field.
void sync_new_dir_from_edit(HWND hwnd, LauncherState* st) {
    wchar_t buf[MAX_PATH * 2] = {};
    ::GetDlgItemTextW(hwnd, kIdNewDir, buf, MAX_PATH * 2);
    const std::string typed = narrow(buf);
    if (!typed.empty()) {
        st->new_dir = typed;
    }
}

// Return/Escape inside the text fields. Called from the message loop BEFORE
// IsDialogMessage — which swallows both keys for dialog navigation, so the
// EDIT subclass alone never sees them — and, as a backup, from edit_subclass
// itself. Returns true when eaten. Actions mirror the buttons exactly:
// Return = first match / Create, Escape = clear the search box.
bool handle_edit_key(HWND hwnd, LauncherState* st, int cid, WPARAM vk) {
    if (st == nullptr) {
        return false;
    }
    if (vk == VK_RETURN) {
        if (cid == kIdSearch) {
            auto filt = filtered_recent(st);
            if (!filt.empty() && st->recent != nullptr) {
                accept_project(hwnd, (*st->recent)[static_cast<std::size_t>(filt[0])]);
            }
        } else if (cid == kIdNewName || cid == kIdNewDir) {
            sync_new_dir_from_edit(hwnd, st);
            on_create(hwnd);
        } else {
            return false;
        }
        return true;
    }
    if (vk == VK_ESCAPE && cid == kIdSearch) {
        ::SetDlgItemTextW(hwnd, kIdSearch, L""); // EN_CHANGE repaints
        return true;
    }
    return false;
}

void show_page(HWND hwnd, LauncherState* st, ShellPage p) {    st->page = p;
    st->hover_nav = -1;
    st->hover_card = -1;
    st->hover_btn = -1;
    layout_children(hwnd, st);
    if (p == ShellPage::Projects) {
        if (HWND s = ::GetDlgItem(hwnd, kIdSearch)) {
            ::SetFocus(s);
        }
    } else if (p == ShellPage::NewProject) {
        if (HWND s = ::GetDlgItem(hwnd, kIdNewName)) {
            ::SetFocus(s);
        }
    }
    ::InvalidateRect(hwnd, nullptr, TRUE);
}

void learn_action(HWND hwnd, LauncherState* st, int i) {
    (void)st;
    switch (i) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5: {
            const std::wstring d = docs_dir();
            if (!d.empty()) {
                open_path(narrow(d));
            } else {
                set_status(hwnd, shell_tr("sh_st_docs_missing"));
            }
            break;
        }
        default:
            break;
    }
    if (i == 1) {
        set_status(hwnd, shell_tr("sh_st_script_soon"));
    }
}

void help_action(HWND hwnd, int i) {
    switch (i) {
        case 0:
        case 1: {
            const std::wstring d = docs_dir();
            if (!d.empty()) {
                open_path(narrow(d));
            } else {
                set_status(hwnd, shell_tr("sh_st_docs_missing"));
            }
            break;
        }
        case 3: {
            const std::wstring r = roadmap_file();
            if (!r.empty()) {
                open_path(narrow(r));
            } else {
                set_status(hwnd, shell_tr("sh_st_roadmap_missing"));
            }
            break;
        }
        default:
            set_status(hwnd, shell_tr("sh_st_community_soon"));
            break;
    }
}

// Live language switch: flips the whole shell NOW (no restart) — g_shell_rtl
// re-mirrors every layout, ui::set_language re-translates every label, native
// children get direction styles, and the choice persists via save_language.
void apply_language(HWND hwnd, LauncherState* st, bool arabic) {
    st->arabic_ui = arabic;
    g_shell_rtl = arabic;
    nf::ui::set_language(arabic ? nf::ui::Language::Arabic : nf::ui::Language::English);
    // Native EDITs: RTL reading order + right alignment for Arabic input; the
    // cue banner and location text itself stay LOGICAL (native shaping, never
    // pre-shaped). Alignment bits are the low two style bits (ES_LEFT=0).
    for (int id : {kIdSearch, kIdNewName, kIdNewDir}) {
        if (HWND h = ::GetDlgItem(hwnd, id)) {
            LONG_PTR ex = ::GetWindowLongPtrW(h, GWL_EXSTYLE);
            if (arabic) {
                ex |= WS_EX_RTLREADING;
            } else {
                ex &= ~static_cast<LONG_PTR>(WS_EX_RTLREADING);
            }
            ::SetWindowLongPtrW(h, GWL_EXSTYLE, ex);
            LONG_PTR stl = ::GetWindowLongPtrW(h, GWL_STYLE);
            stl = (stl & ~static_cast<LONG_PTR>(3)) | (arabic ? ES_RIGHT : ES_LEFT);
            ::SetWindowLongPtrW(h, GWL_STYLE, stl);
            ::SetWindowPos(h, nullptr, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            ::InvalidateRect(h, nullptr, TRUE);
        }
    }
    if (HWND s = ::GetDlgItem(hwnd, kIdSearch)) {
        // Cue banner is native chrome: LOGICAL text (the control shapes it).
        ::SendMessageW(s, EM_SETCUEBANNER, TRUE,
                       reinterpret_cast<LPARAM>(widen(shell_tr("sh_search_placeholder")).c_str()));
    }
    // The status line is owner-drawn (see draw_status): alignment flips there
    // with g_shell_rtl — no SS_RIGHT/SS_LEFT style touch, which would strip the
    // SS_OWNERDRAW type bits.
    save_language(arabic);
    layout_children(hwnd, st); // EDIT/status rects follow the mirrored layout
    ::InvalidateRect(hwnd, nullptr, TRUE);
}

void do_action(HWND hwnd, LauncherState* st, const Hit& h) {
    switch (h.kind) {
        case Hit::Kind::None:
            return;
        case Hit::Kind::Nav:
            show_page(hwnd, st, static_cast<ShellPage>(h.index));
            return;
        case Hit::Kind::OpenFile: {
            std::string picked;
            // Native dialog: LOGICAL title (the OS shapes its own chrome).
            if (pick_project_file_dialog(shell_tr("sh_dialog_open_project"), st->new_dir, picked,
                                          hwnd)) {
                accept_project(hwnd, picked);
            }
            return;
        }
        case Hit::Kind::NewPage:
            if (h.index == -1) {
                show_page(hwnd, st, ShellPage::Projects); // Cancel
            } else {
                show_page(hwnd, st, ShellPage::NewProject);
            }
            return;
        case Hit::Kind::ProjectCard: {
            if (st->recent != nullptr && h.index >= 0 &&
                static_cast<std::size_t>(h.index) < st->recent->size()) {
                accept_project(hwnd, (*st->recent)[static_cast<std::size_t>(h.index)]);
            }
            return;
        }
        case Hit::Kind::FeaturedOpen: {
            auto filt = filtered_recent(st);
            if (!filt.empty() && st->recent != nullptr) {
                accept_project(hwnd, (*st->recent)[static_cast<std::size_t>(filt[0])]);
            }
            return;
        }
        case Hit::Kind::FeaturedFolder: {
            auto filt = filtered_recent(st);
            if (!filt.empty() && st->recent != nullptr) {
                const std::string& p = (*st->recent)[static_cast<std::size_t>(filt[0])];
                const std::string dir = std::filesystem::path(p).parent_path().string();
                // A stale entry's folder is gone too: say so instead of
                // ShellExecute-ing a dead path (which fails silently).
                std::error_code ec;
                if (!std::filesystem::exists(dir, ec)) {
                    set_status(hwnd, shell_tr("sh_st_missing") + "\n" + p);
                    return;
                }
                open_path(dir);
            }
            return;
        }
        case Hit::Kind::DimTab:
        case Hit::Kind::DimBtn:
            st->project_dim = h.index;
            if (h.index == 1) { // 2D
                st->selected_template = 1; // Platformer2D
            } else if (h.index == 2) { // 3D
                if (st->selected_template == 1) {
                    st->selected_template = 3; // ThirdPerson
                }
            }
            ::InvalidateRect(hwnd, nullptr, TRUE);
            return;
        case Hit::Kind::TemplateCard:
            st->selected_template = h.index;
            if (h.index == 1) {
                st->project_dim = 1; // 2D
            } else if (h.index == 2 || h.index == 3) {
                st->project_dim = 2; // 3D
            } else if (h.index == 4) {
                st->project_dim = 0; // Hybrid
            }
            ::InvalidateRect(hwnd, nullptr, TRUE);
            return;
        case Hit::Kind::Browse: {
            std::string picked;
            if (pick_folder_dialog(shell_tr("sh_dialog_choose_loc"), st->new_dir, picked,
                                    hwnd)) {
                st->new_dir = picked;
                ::SetDlgItemTextW(hwnd, kIdNewDir, widen(picked).c_str());
            }
            return;
        }
        case Hit::Kind::Create: {
            sync_new_dir_from_edit(hwnd, st);
            on_create(hwnd);
            return;
        }
        case Hit::Kind::LearnCard: {
            // Disabled ("soon") cards must not act: they explain themselves.
            if (h.index < 0 || h.index >= 6 || !kLearn[h.index].enabled) {
                set_status(hwnd, shell_tr("sh_st_script_soon"));
                return;
            }
            learn_action(hwnd, st, h.index);
            return;
        }
        case Hit::Kind::HelpCard: {
            if (h.index < 0 || h.index >= 6 || !kHelp[h.index].enabled) {
                set_status(hwnd, shell_tr("sh_st_community_soon"));
                return;
            }
            help_action(hwnd, h.index);
            return;
        }
        case Hit::Kind::LangEn:
            apply_language(hwnd, st, false);
            return;
        case Hit::Kind::LangAr:
            apply_language(hwnd, st, true);
            return;
        case Hit::Kind::ClearRecent: {
            // Irreversible (the caller persists on return): confirm first.
            // Button captions come from the OS locale — the question itself is
            // translated, which is the part the shell owns.
            const int ans = ::MessageBoxW(
                hwnd, widen(shell_tr("sh_confirm_clear_text")).c_str(),
                widen(shell_tr("sh_confirm_clear_title")).c_str(), MB_YESNO | MB_ICONQUESTION);
            if (ans != IDYES) {
                return;
            }
            if (st->recent != nullptr) {
                // Caller owns the vector (non-const ref) and saves after we
                // return, so clearing here persists.
                const_cast<std::vector<std::string>*>(st->recent)->clear();
                set_status(hwnd, shell_tr("sh_st_cleared"), theme::ok);
                ::InvalidateRect(hwnd, nullptr, TRUE);
            }
            return;
        }
        case Hit::Kind::SearchClear:
            // EN_CHANGE on the edit refreshes st->search and repaints; focus
            // stays in the box so the next query starts typing immediately.
            ::SetDlgItemTextW(hwnd, kIdSearch, L"");
            if (HWND s = ::GetDlgItem(hwnd, kIdSearch)) {
                ::SetFocus(s);
            }
            return;
        case Hit::Kind::RemoveCard:
            if (st->recent != nullptr && h.index >= 0 &&
                static_cast<std::size_t>(h.index) < st->recent->size()) {
                // Same ownership as ClearRecent: the caller saves afterwards.
                const_cast<std::vector<std::string>*>(st->recent)
                    ->erase(st->recent->begin() + h.index);
                st->hover_card = -1;
                st->hover_btn = -1; // indices shifted — drop stale hover
                set_status(hwnd, shell_tr("sh_st_removed"), theme::text_dim);
                ::InvalidateRect(hwnd, nullptr, TRUE);
            }
            return;
        case Hit::Kind::DocsButton: {
            const std::wstring d = docs_dir();
            if (!d.empty()) {
                open_path(narrow(d));
            } else {
                set_status(hwnd, shell_tr("sh_st_docs_missing"));
            }
            return;
        }
        case Hit::Kind::BugButton:
            open_url("https://github.com/abdallah2183/SANAD-Engine/issues");
            return;
    }
}

} // namespace shell

LRESULT CALLBACK shell_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LauncherState* st = state_of(hwnd);
    switch (msg) {
        case WM_ERASEBKGND:
            return 1; // painted in WM_PAINT
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC wdc = ::BeginPaint(hwnd, &ps);
            RECT rc;
            ::GetClientRect(hwnd, &rc);
            const int W = rc.right - rc.left;
            const int H = rc.bottom - rc.top;
            // Double-buffered: the whole shell paints to a memory bitmap and
            // lands with one BitBlt, so page switches never flash. (Direct
            // painting flashed because every switch repainted ~10 StretchBlts
            // straight to the screen.)
            HDC dc = ::CreateCompatibleDC(wdc);
            HBITMAP buf = ::CreateCompatibleBitmap(wdc, (W > 0) ? W : 1, (H > 0) ? H : 1);
            HGDIOBJ old_bmp = ::SelectObject(dc, buf);
            HBRUSH bg = (st != nullptr && st->brush_bg != nullptr)
                            ? st->brush_bg
                            : ::CreateSolidBrush(theme::bg);
            const bool owned = (st == nullptr || st->brush_bg == nullptr);
            ::FillRect(dc, &rc, bg);
            if (owned) {
                ::DeleteObject(bg);
            }
            if (st != nullptr) {
                shell::paint_sidebar(dc, st, W, H);
                using shell::content_x;
                switch (st->page) {
                    case ShellPage::Projects: {
                        auto filt = shell::filtered_recent(st);
                        shell::ProjectsLayout L = shell::layout_projects(st, W);
                        shell::paint_projects(dc, st, W, H, L, filt);
                        break;
                    }
                    case ShellPage::NewProject: {
                        shell::NewLayout L = shell::layout_new(st, W);
                        shell::paint_new(dc, st, W, L);
                        break;
                    }
                    case ShellPage::Learn:
                        shell::paint_card_grid(dc, st, W, shell::kLearn, st->hover_card);
                        shell::paint_page_head(dc, st, W, "sh_learn_title", nullptr);
                        break;
                    case ShellPage::AssetStore:
                        shell::paint_store(dc, st, W);
                        break;
                    case ShellPage::Settings:
                        shell::paint_settings(dc, st, W);
                        break;
                    case ShellPage::Help:
                        shell::paint_card_grid(dc, st, W, shell::kHelp, st->hover_card);
                        shell::paint_page_head(dc, st, W, "sh_help_title", nullptr);
                        break;
                }
            }
            ::BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
            ::SelectObject(dc, old_bmp);
            ::DeleteObject(buf);
            ::DeleteDC(dc);
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            // The window is resizable now: the hand-drawn pages already derive
            // from the client rect at paint time, but the EDIT children and the
            // status line are real windows and have to be moved explicitly.
            // The DPI refresh keeps fonts crisp when the window moves across
            // monitors (rebuilds only on change, so resize drags stay cheap).
            if (st != nullptr) {
                refresh_dpi(hwnd, st);
                shell::layout_children(hwnd, st);
                ::InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_MOVE:
            // Monitor changes arrive here for system-DPI-aware windows (which
            // never see WM_DPICHANGED): re-read the window's own DPI.
            if (st != nullptr) {
                refresh_dpi(hwnd, st);
                ::InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_DPICHANGED:
            // PerMonitor-aware future (or a manifest bump): the OS hands the
            // new DPI plus the suggested rect — take both.
            if (st != nullptr) {
                const UINT dpi = LOWORD(wp) != 0 ? static_cast<UINT>(LOWORD(wp)) : st->dpi;
                if (dpi != st->dpi) {
                    st->dpi = dpi;
                    create_state_fonts(st);
                    restyle_children(hwnd, st);
                }
                if (const RECT* suggested = reinterpret_cast<const RECT*>(lp)) {
                    ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                                   suggested->right - suggested->left,
                                   suggested->bottom - suggested->top,
                                   SWP_NOZORDER | SWP_NOACTIVATE);
                }
                shell::layout_children(hwnd, st);
                ::InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_GETMINMAXINFO:
            // Never let the window shrink past the layout's own assumptions:
            // the Projects header row needs ~1080 px of content and the New
            // Project form plus the status line need ~720 px of height.
            if (st != nullptr) {
                MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lp);
                mmi->ptMinTrackSize.x = scale_dpi(st, 1080);
                mmi->ptMinTrackSize.y = scale_dpi(st, 720);
            }
            break; // DefWindowProc keeps the system's other defaults
        case WM_LBUTTONDOWN: {
            if (st == nullptr) {
                break;
            }
            RECT rc;
            ::GetClientRect(hwnd, &rc);
            const int W = rc.right - rc.left;
            const int H = rc.bottom - rc.top;
            const int x = static_cast<int>(static_cast<short>(LOWORD(lp)));
            const int y = static_cast<int>(static_cast<short>(HIWORD(lp)));
            const shell::Hit hit = shell::hit_test(st, x, y, W, H);
            shell::do_action(hwnd, st, hit);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (st == nullptr) {
                break;
            }
            RECT rc;
            ::GetClientRect(hwnd, &rc);
            const int W = rc.right - rc.left;
            const int H = rc.bottom - rc.top;
            const int x = static_cast<int>(static_cast<short>(LOWORD(lp)));
            const int y = static_cast<int>(static_cast<short>(HIWORD(lp)));
            shell::Hit h = shell::hit_test(st, x, y, W, H);
            int nav = -1, card = -1, btn = -1;
            if (h.kind == shell::Hit::Kind::Nav) {
                nav = h.index;
            } else if (h.kind == shell::Hit::Kind::ProjectCard ||
                       h.kind == shell::Hit::Kind::TemplateCard ||
                       h.kind == shell::Hit::Kind::LearnCard ||
                       h.kind == shell::Hit::Kind::HelpCard) {
                card = h.index;
            } else if (h.kind == shell::Hit::Kind::DimTab) {
                btn = 20 + h.index;
            } else if (h.kind == shell::Hit::Kind::DimBtn) {
                btn = 30 + h.index;
            } else if (h.kind == shell::Hit::Kind::LangEn || h.kind == shell::Hit::Kind::LangAr) {
                card = 100; // the General Preferences card highlights with its radios
            } else {
                // Painters compare hover_btn against small per-page ids (see
                // the paint_* call sites), not the Kind enum — translate here
                // so highlight and cursor agree with what was drawn.
                switch (h.kind) {
                    case shell::Hit::Kind::OpenFile:
                        btn = 1;
                        break;
                    case shell::Hit::Kind::NewPage:
                        btn = 2;
                        break;
                    case shell::Hit::Kind::FeaturedOpen:
                        btn = 3;
                        break;
                    case shell::Hit::Kind::FeaturedFolder:
                        btn = 4;
                        break;
                    case shell::Hit::Kind::SearchClear:
                        btn = 5;
                        break;
                    case shell::Hit::Kind::RemoveCard:
                        btn = 6;
                        card = h.index; // the card glows with its ×
                        break;
                    case shell::Hit::Kind::Browse:
                        btn = 11;
                        break;
                    case shell::Hit::Kind::Create:
                        btn = 12;
                        break;
                    case shell::Hit::Kind::ClearRecent:
                        btn = 21;
                        break;
                    default:
                        break;
                }
                // Cancel shares the NewPage kind with index -1.
                if (h.kind == shell::Hit::Kind::NewPage && h.index == -1) {
                    btn = 13;
                }
            }
            if (nav != st->hover_nav || card != st->hover_card || btn != st->hover_btn) {
                st->hover_nav = nav;
                st->hover_card = card;
                st->hover_btn = btn;
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_SETCURSOR: {
            if (st != nullptr && (st->hover_nav >= 0 || st->hover_card >= 0 || st->hover_btn >= 0)) {
                ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32649))); // IDC_HAND
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == kIdSearch && code == EN_CHANGE && st != nullptr) {
                shell::read_search_edit(hwnd, st);
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_DROPFILES: {
            // Drop a project file anywhere on the shell to open it.
            HDROP drop = reinterpret_cast<HDROP>(wp);
            bool opened = false;
            if (st != nullptr && ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) == 1) {
                wchar_t path[MAX_PATH * 2] = {};
                if (::DragQueryFileW(drop, 0, path, MAX_PATH * 2) != 0) {
                    const std::string picked = narrow(path);
                    std::error_code ec;
                    if (shell::has_nfproj_ext(picked) && std::filesystem::exists(picked, ec)) {
                        accept_project(hwnd, picked);
                        opened = true;
                    }
                }
            }
            ::DragFinish(drop);
            if (!opened && st != nullptr) {
                set_status(hwnd, shell_tr("sh_st_drop_only"));
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HWND ctl = reinterpret_cast<HWND>(lp);
            HDC hdc = reinterpret_cast<HDC>(wp);
            LauncherState* s2 = state_of(hwnd);
            const int id = ::GetDlgCtrlID(ctl);
            ::SetBkMode(hdc, TRANSPARENT);
            if (id == kIdStatus) {
                ::SetTextColor(hdc, s2 != nullptr ? s2->status_color : theme::error);
            } else {
                ::SetTextColor(hdc, theme::text_dim);
            }
            return reinterpret_cast<LRESULT>(s2 != nullptr && s2->brush_bg != nullptr
                                                  ? s2->brush_bg
                                                  : ::GetStockObject(BLACK_BRUSH));
        }
        case WM_DRAWITEM: {
            // Owner-draw statics (the status line) report here like the buttons.
            const DRAWITEMSTRUCT* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (dis != nullptr && dis->CtlType == ODT_STATIC && dis->CtlID == kIdStatus) {
                shell::draw_status(dis, st);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            LauncherState* s2 = state_of(hwnd);
            ::SetTextColor(hdc, theme::text);
            ::SetBkColor(hdc, theme::list_bg);
            return reinterpret_cast<LRESULT>(s2 != nullptr && s2->brush_list != nullptr
                                                  ? s2->brush_list
                                                  : ::GetStockObject(BLACK_BRUSH));
        }
        case WM_CLOSE:
            if (st != nullptr) {
                st->result.quit = true;
                st->done = true;
            }
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

#ifdef _WIN32

LRESULT CALLBACK btn_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR /*id*/,
                              DWORD_PTR /*ref*/) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            if (::GetPropW(hwnd, kHoverProp) == nullptr) {
                ::SetPropW(hwnd, kHoverProp, reinterpret_cast<HANDLE>(1));
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                ::TrackMouseEvent(&tme);
                ::InvalidateRect(hwnd, nullptr, TRUE);
            }
            return ::DefSubclassProc(hwnd, msg, wp, lp);
        }
        case WM_MOUSELEAVE:
            ::RemovePropW(hwnd, kHoverProp);
            if (BtnExtra* ex = reinterpret_cast<BtnExtra*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
                ex->hover = false;
            }
            ::InvalidateRect(hwnd, nullptr, TRUE);
            return ::DefSubclassProc(hwnd, msg, wp, lp);
        case WM_NCDESTROY: {
            if (BtnExtra* ex = reinterpret_cast<BtnExtra*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
                delete ex;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            break;
        }
        default:
            break;
    }
    return ::DefSubclassProc(hwnd, msg, wp, lp);
}

HWND add_label(HWND parent, const wchar_t* text, int x, int y, int w, int h, HFONT font,
               int id = -1) {
    HWND ctl = ::CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, w, h,
                                 parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr,
                                 nullptr);
    if (ctl != nullptr && font != nullptr) {
        ::SendMessageW(ctl, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    return ctl;
}

// Keyboard in the text fields: single-line EDITs swallow Return (beep) and
// ignore Escape, so both are handled here. ref carries the parent shell HWND,
// id carries the control id. Return activates the page's default action
// (first match / Create); Escape clears the search box.
LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                               DWORD_PTR ref) {
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        HWND parent = reinterpret_cast<HWND>(ref);
        LauncherState* st = state_of(parent);
        if (shell::handle_edit_key(parent, st, static_cast<int>(id), wp)) {
            return 0;
        }
    }
    if (msg == WM_CHAR || msg == WM_KEYDOWN || msg == WM_SETTEXT || msg == WM_PASTE ||
        msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        const LRESULT r = ::DefSubclassProc(hwnd, msg, wp, lp);
        ::InvalidateRect(hwnd, nullptr, TRUE);
        return r;
    }
    // Native EDIT paints its text with DrawText, which is blank on this
    // Windows session. Keep the native editing model (keyboard, selection,
    // caret, scrolling) and repaint the client with ExtTextOutW ourselves.
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = ::BeginPaint(hwnd, &ps);
        RECT rc;
        ::GetClientRect(hwnd, &rc);
        if (rc.right > rc.left && rc.bottom > rc.top) {
            HBRUSH bg = ::CreateSolidBrush(theme::list_bg);
            ::FillRect(dc, &rc, bg);
            ::DeleteObject(bg);
            const bool focused = (::GetFocus() == hwnd);
            HBRUSH frame_br = ::CreateSolidBrush(focused ? theme::accent : theme::border);
            ::FrameRect(dc, &rc, frame_br);
            ::DeleteObject(frame_br);

            wchar_t text[1024] = {};
            const int len = ::GetWindowTextW(hwnd, text, 1024);
            std::wstring shown;
            COLORREF color = theme::text;
            if (len > 0) {
                shown = shell_w_str(narrow(text));
            } else {
                wchar_t cue[256] = {};
                if (::SendMessageW(hwnd, EM_GETCUEBANNER,
                                   reinterpret_cast<WPARAM>(cue), 256) != FALSE) {
                    shown = shell_w_str(narrow(cue));
                    color = theme::text_dim;
                }
            }
            const HFONT font = reinterpret_cast<HFONT>(
                ::SendMessageW(hwnd, WM_GETFONT, 0, 0));
            UINT fmt = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
            if (g_shell_rtl) {
                fmt = (fmt & static_cast<UINT>(~DT_LEFT)) | DT_RIGHT;
            }
            draw_text_ext(dc, shown.c_str(), rc.left + 8, rc.top,
                          rc.right - rc.left - 16, rc.bottom - rc.top, font, color, fmt);
        }
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    return ::DefSubclassProc(hwnd, msg, wp, lp);
}

HWND add_control(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y,
                 int w, int h, int id, HFONT font = nullptr) {
    HWND ctl = ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr,
                                 nullptr);
    if (ctl != nullptr && font != nullptr) {
        ::SendMessageW(ctl, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    return ctl;
}

HWND add_button(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id, BtnRole role,
                HFONT font, bool is_default = false) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_PUSHBUTTON;
    if (is_default) {
        style |= BS_DEFPUSHBUTTON;
    }
    HWND ctl = ::CreateWindowExW(0, L"BUTTON", text, style, x, y, w, h, parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr,
                                 nullptr);
    if (ctl == nullptr) {
        return nullptr;
    }
    auto* ex = new BtnExtra{};
    ex->role = role;
    ex->font = font;
    ::SetWindowLongPtrW(ctl, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ex));
    ::SetWindowSubclass(ctl, btn_subclass, 1, 0);
    if (font != nullptr) {
        ::SendMessageW(ctl, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    return ctl;
}

#endif // _WIN32

LauncherResult run_project_launcher(std::vector<std::string>& recent_projects) {
    LauncherResult result;
#ifdef _WIN32
    // IFileDialog is COM; nothing else in the editor initialises COM, so this is
    // the only place it happens and it must be matched by CoUninitialize below.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool com_ok = SUCCEEDED(com);

    // Common controls (SetWindowSubclass lives in comctl32).
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    ::InitCommonControlsEx(&icc);

    const wchar_t* kClass = L"SANADProjectLauncher";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = shell_wnd_proc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    // MAKEINTRESOURCEW, not IDC_ARROW: this TU is built without UNICODE defined
    // (the engine is ANSI-agnostic and nothing else needs it), so the IDC_* macro
    // would expand to the A form and LoadCursorW would reject it.
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    // Application icon (resource id 1, see Editor/resources/version.rc.in) for
    // the title bar, taskbar and Alt-Tab. Without these the launcher showed the
    // blank default glyph even though the exe carries the SANAD logo.
    wc.hIcon = static_cast<HICON>(::LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                               ::GetSystemMetrics(SM_CXICON),
                                               ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(::LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                 ::GetSystemMetrics(SM_CXSMICON),
                                                 ::GetSystemMetrics(SM_CYSMICON),
                                                 LR_DEFAULTCOLOR));
    wc.hbrBackground = nullptr;                                  // painted in WM_PAINT
    wc.lpszClassName = kClass;
    ::RegisterClassExW(&wc);

    LauncherState state;
    state.recent = &recent_projects;
    // First run has no projects to list. Landing on the empty Projects grid
    // made "where do I start?" a real question; go straight to the New Project
    // page (template gallery + name + location) when there is nothing to open.
    if (recent_projects.empty()) {
        state.page = ShellPage::NewProject;
    }
    state.new_dir = known_documents_dir();
    // A downloaded-and-extracted ZIP is marked Low integrity by the browser,
    // and Windows then denies writes to Desktop/Documents. Keep the launcher
    // usable by pointing the form at a directory the process can really write,
    // and surface the exact repair steps in the status line.
    state.low_integrity = process_is_low_integrity();
    if (state.low_integrity && !directory_is_writable(state.new_dir)) {
        state.fallback_dir = fallback_project_dir();
        if (!state.fallback_dir.empty()) {
            state.new_dir = state.fallback_dir;
        }
    }
    state.status_color = theme::text_dim;

    // Restored choice: the shell reopens in the saved language, and the RTL
    // flag is set BEFORE any paint so the first frame is already mirrored.
    const LauncherSettings boot = load_settings();
    state.arabic_ui = boot.arabic;
    g_shell_rtl = boot.arabic;
    nf::ui::set_language(boot.arabic ? nf::ui::Language::Arabic : nf::ui::Language::English);

    // DPI — per-monitor text metrics so the launcher is crisp on scaled displays.
    state.dpi = 96;
    // The window does not exist yet; use the DC of the primary screen.
    if (HDC screen = ::GetDC(nullptr)) {
        const UINT dpi = static_cast<UINT>(::GetDeviceCaps(screen, LOGPIXELSX));
        if (dpi != 0) {
            state.dpi = dpi;
        }
        ::ReleaseDC(nullptr, screen);
    }
    // Display face first: create_state_fonts picks it up for headings.
    g_amiri_path = find_font_file();
    if (!g_amiri_path.empty() &&
        ::AddFontResourceExW(g_amiri_path.c_str(), FR_PRIVATE, nullptr) != 0) {
        g_amiri_ok = true;
    } else {
        ::OutputDebugStringA("[SANAD] Amiri display font NOT loaded — Arabic "
                             "headings fall back to Segoe UI\n");
    }
    create_state_fonts(&state);
    state.brush_bg = ::CreateSolidBrush(theme::bg);
    state.brush_surface = ::CreateSolidBrush(theme::surface);
    state.brush_list = ::CreateSolidBrush(theme::list_bg);
    state.brush_sel = ::CreateSolidBrush(theme::sel_bg);

    const int sx = static_cast<int>(state.dpi) / 96;
    int W = 1180 * sx;
    int H = 720 * sx;
    // Restored window state: maximized by default (and on first run), else the
    // saved outer size. Sizes are outer (frame included) both ways, so restore
    // reuses them directly while first-run still goes through AdjustWindowRect.
    bool start_maxed = true;
    RECT wrc = {0, 0, W, H};
    // Resizable shell with the full caption button set. Without
    // WS_MAXIMIZEBOX/WS_MINIMIZEBOX the title bar renders only the close
    // button and there is no way to maximise the launcher at all (it was a
    // fixed-size window); WS_THICKFRAME additionally enables drag-resize and
    // the Aero snap / double-click-caption behaviours. Layout is computed from
    // the client rect at paint/click time and WM_SIZE repositions the EDIT
    // children, so growing the window is handled end to end.
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
                  WS_THICKFRAME | WS_VISIBLE;
    if (!boot.maximized && boot.width >= 800 && boot.height >= 500 && boot.width <= 7680 &&
        boot.height <= 4320) {
        // Saved outer size from the previous session (captured at exit).
        wrc.right = (boot.width * static_cast<int>(state.dpi)) / 96;
        wrc.bottom = (boot.height * static_cast<int>(state.dpi)) / 96;
        start_maxed = false;
    } else {
        ::AdjustWindowRectEx(&wrc, style, FALSE, 0);
    }

    // Centre on the work area so the launcher does not land under the taskbar.
    RECT work{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - (wrc.right - wrc.left)) / 2;
    const int win_y = work.top + ((work.bottom - work.top) - (wrc.bottom - wrc.top)) / 2;

    HWND hwnd = ::CreateWindowExW(0, kClass, L"SANAD — Project", style, x, win_y,
                                  wrc.right - wrc.left, wrc.bottom - wrc.top, nullptr, nullptr,
                                  wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        destroy_state_fonts(&state);
        if (g_amiri_ok && !g_amiri_path.empty()) {
            ::RemoveFontResourceExW(g_amiri_path.c_str(), FR_PRIVATE, nullptr);
            g_amiri_ok = false;
            g_amiri_path.clear();
        }
        if (state.brush_bg != nullptr) {
            ::DeleteObject(state.brush_bg);
        }
        if (state.brush_surface != nullptr) {
            ::DeleteObject(state.brush_surface);
        }
        if (state.brush_list != nullptr) {
            ::DeleteObject(state.brush_list);
        }
        if (state.brush_sel != nullptr) {
            ::DeleteObject(state.brush_sel);
        }
        if (com_ok) {
            ::CoUninitialize();
        }
        result.quit = true;
        return result;
    }
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

    // Dark caption to match the forge theme (white title bar clashed with the
    // dark shell) + rounded corners on Win11. LoadLibrary, not a link dep, so
    // older SDKs still compile; every failure falls back to the stock caption.
    if (HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll")) {
        using SetAttrFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        auto set_attr =
            reinterpret_cast<SetAttrFn>(::GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (set_attr != nullptr) {
            const BOOL dark = TRUE;
            set_attr(hwnd, 20, &dark, sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE
            const COLORREF cap = RGB(16, 16, 20);
            set_attr(hwnd, 35, &cap, sizeof(cap)); // DWMWA_CAPTION_COLOR (Win11+)
            const DWORD corners = 2;               // DWMWCP_ROUND (Win11+)
            set_attr(hwnd, 33, &corners, sizeof(corners));
        }
        ::FreeLibrary(dwm);
    }
    // Project files can be dropped anywhere on the shell (see WM_DROPFILES).
    ::DragAcceptFiles(hwnd, TRUE);

    // The shell is a full workspace, not a dialog: start maximized like the
    // editor does unless the previous session left it restored — and (now that
    // the style carries WS_MAXIMIZEBOX and WS_THICKFRAME) the user can
    // restore, resize or re-maximise it freely. WM_SIZE keeps the child
    // controls in step with the hand-drawn layout.
    ::ShowWindow(hwnd, start_maxed ? SW_MAXIMIZE : SW_SHOWNORMAL);

    // GDI+ for PNG thumbnails (shell runs before the GPU device exists).
    Gdiplus::GdiplusStartupInput gsi;
    if (Gdiplus::GdiplusStartup(&state.gdiplus_token, &gsi, nullptr) != Gdiplus::Ok) {
        state.gdiplus_token = 0;
    }
    if (state.gdiplus_token != 0) {
        const wchar_t* files[4] = {L"shot_game.png", L"shot_vehicle.png", L"shot_editor_en.png",
                                   L"shot_editor_ar.png"};
        for (int i = 0; i < 4; ++i) {
            const std::wstring p = shell::find_doc_image(files[i]);
            state.thumbs[i].bmp = shell::load_thumb(p, state.thumbs[i].w, state.thumbs[i].h);
        }
        state.logo.bmp =
            shell::load_thumb(shell::find_logo_image(), state.logo.w, state.logo.h);
        // The window was shown (and painted once) before GDI+ came up; repaint
        // so the first visible frame already carries the logo and thumbnails.
        ::InvalidateRect(hwnd, nullptr, FALSE);
    }

    // Shell controls: three text inputs (search on Projects, name + location
    // on New) plus the status line. Everything else clickable is a manual
    // hit rect painted per page, positioned by layout_children().
    // Native chrome takes LOGICAL text (the OS shapes it); only hand-drawn
    // paint goes through the shaping path. Alignment matches the live toggle
    // in apply_language (ES_RIGHT under RTL).
    const DWORD edit_align = g_shell_rtl ? ES_RIGHT : ES_LEFT;
    const DWORD edit_ex = g_shell_rtl ? WS_EX_RTLREADING : 0;
    HWND search_edit = ::CreateWindowExW(
        edit_ex, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | edit_align, 0, 0, 10,
        10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSearch)), nullptr, nullptr);
    if (search_edit != nullptr && state.font_body != nullptr) {
        ::SendMessageW(search_edit, WM_SETFONT, reinterpret_cast<WPARAM>(state.font_body), TRUE);
        // TRUE = show the hint even while the box has focus. The launcher
        // focuses the search box on startup, so the default (unfocused only)
        // would leave the box looking like an unlabelled empty rectangle —
        // exactly what it did before this line existed.
        ::SendMessageW(search_edit, EM_SETCUEBANNER, TRUE,
                       reinterpret_cast<LPARAM>(
                           widen(shell_tr("sh_search_placeholder")).c_str()));
        // Return/Escape handling for every text field (see edit_subclass).
        ::SetWindowSubclass(search_edit, edit_subclass, kIdSearch,
                            reinterpret_cast<DWORD_PTR>(hwnd));
    }
    add_control(hwnd, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | edit_align, 0, 0,
                10, 10, kIdNewName, state.font_body);
    HWND loc_edit =
        add_control(hwnd, L"EDIT", widen(state.new_dir).c_str(),
                    WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | edit_align, 0, 0, 10, 10, kIdNewDir,
                    state.font_body);
    // Example name hint + Return/Escape handling on both New-page fields.
    if (HWND name_edit = ::GetDlgItem(hwnd, kIdNewName)) {
        ::SendMessageW(name_edit, EM_SETCUEBANNER, TRUE,
                       reinterpret_cast<LPARAM>(widen(shell_tr("sh_name_hint")).c_str()));
        ::SetWindowSubclass(name_edit, edit_subclass, kIdNewName,
                            reinterpret_cast<DWORD_PTR>(hwnd));
    }
    if (loc_edit != nullptr) {
        ::SetWindowSubclass(loc_edit, edit_subclass, kIdNewDir,
                            reinterpret_cast<DWORD_PTR>(hwnd));
    }
    if (g_shell_rtl) {
        // Same direction the live toggle applies (see apply_language): Arabic
        // input reads right-to-left from the first keystroke.
        for (HWND h : {search_edit, ::GetDlgItem(hwnd, kIdNewName), loc_edit}) {
            if (h != nullptr) {
                const LONG_PTR ex = ::GetWindowLongPtrW(h, GWL_EXSTYLE);
                ::SetWindowLongPtrW(h, GWL_EXSTYLE, ex | WS_EX_RTLREADING);
            }
        }
    }
    {
        RECT crc;
        ::GetClientRect(hwnd, &crc);
        const int cW = crc.right - crc.left;
        const int cH = crc.bottom - crc.top;
        // Status: OWNERDRAW — mixed Arabic/Latin text scrambles under the
        // native STATIC draw (draw_status repaints it through the ExtTextOut
        // bidi path instead). Alignment flips inside draw_status with
        // g_shell_rtl, so no per-language style flip is applied here.
        add_control(hwnd, L"STATIC", L"", SS_OWNERDRAW,
                    shell::content_x(&state), cH - scale_dpi(&state, 72),
                    shell::content_w(&state, cW), scale_dpi(&state, 58), kIdStatus,
                    state.font_small);
    }

    // Do NOT snap back to Projects here: launch() already picked New Project
    // when the recent list is empty, and this reset was silently overriding it
    // (the empty library still landed on a Projects page with nothing to open).
    shell::layout_children(hwnd, &state);
    if (state.low_integrity) {
        // A silent read-only Documents folder made Create look like a dead
        // button. Say what Windows did and the one command that repairs it.
        std::string warning = shell_tr("sh_st_low_integrity");
        if (!state.fallback_dir.empty()) {
            warning += "\n" + shell_tr("sh_st_low_integrity_dir") + " " + state.fallback_dir;
        }
        set_status(hwnd, warning, theme::error);
    }
    if (HWND focus = ::GetDlgItem(hwnd, state.page == ShellPage::Projects ? kIdSearch
                                                                         : kIdNewName)) {
        ::SetFocus(focus);
    }

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Shell accelerators (checked before IsDialogMessage eats the keys):
        // Ctrl+F focuses the Projects search box; Alt+1..6 jumps straight to
        // a page. Both are ignored while a modal file dialog owns input (its
        // own message loop runs then, not this one).
        if (msg.message == WM_KEYDOWN && (::GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
            msg.wParam == 'F') {
            if (state.page == ShellPage::Projects) {
                if (HWND s = ::GetDlgItem(hwnd, kIdSearch)) {
                    ::SetFocus(s);
                    ::SendMessageW(s, EM_SETSEL, 0, -1);
                }
                continue;
            }
        }
        if (msg.message == WM_SYSKEYDOWN && msg.wParam >= '1' && msg.wParam <= '6') {
            shell::show_page(hwnd, &state,
                             static_cast<ShellPage>(msg.wParam - '1'));
            continue;
        }
        // Text-field keys before IsDialogMessage eats them (see
        // handle_edit_key): only when focus sits in one of our EDITs.
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE)) {
            HWND focus = ::GetFocus();
            const int id = (focus != nullptr) ? ::GetDlgCtrlID(focus) : -1;
            if ((id == kIdSearch || id == kIdNewName || id == kIdNewDir) &&
                shell::handle_edit_key(hwnd, &state, id, msg.wParam)) {
                continue;
            }
        }
        if (!::IsDialogMessageW(hwnd, &msg)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }
    // Persist window state for the next launch (language was already saved
    // live by apply_language; reload-then-store keeps it while updating the
    // geometry from the real final placement).
    {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        LauncherSettings cur = load_settings();
        cur.arabic = state.arabic_ui;
        if (::GetWindowPlacement(hwnd, &wp) != 0) {
            cur.maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
            if (!cur.maximized) {
                const int ow = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
                const int oh = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
                if (ow >= 800 && oh >= 500 && ow <= 7680 && oh <= 4320) {
                    cur.width = (ow * 96) / static_cast<int>(state.dpi != 0 ? state.dpi : 96);
                    cur.height = (oh * 96) / static_cast<int>(state.dpi != 0 ? state.dpi : 96);
                }
            }
        }
        save_settings(cur);
    }
    ::DestroyWindow(hwnd);

    // Fonts and brushes are not selected into any live DC at this point.
    destroy_state_fonts(&state);
    if (g_amiri_ok && !g_amiri_path.empty()) {
        ::RemoveFontResourceExW(g_amiri_path.c_str(), FR_PRIVATE, nullptr);
        g_amiri_ok = false;
        g_amiri_path.clear();
    }
    for (int i = 0; i < 4; ++i) {
        if (state.thumbs[i].bmp != nullptr) {
            ::DeleteObject(state.thumbs[i].bmp);
            state.thumbs[i].bmp = nullptr;
        }
    }
    if (state.logo.bmp != nullptr) {
        ::DeleteObject(state.logo.bmp);
        state.logo.bmp = nullptr;
    }
    if (state.gdiplus_token != 0) {
        Gdiplus::GdiplusShutdown(state.gdiplus_token);
        state.gdiplus_token = 0;
    }
    if (state.brush_bg != nullptr) {
        ::DeleteObject(state.brush_bg);
    }
    if (state.brush_surface != nullptr) {
        ::DeleteObject(state.brush_surface);
    }
    if (state.brush_list != nullptr) {
        ::DeleteObject(state.brush_list);
    }
    if (state.brush_sel != nullptr) {
        ::DeleteObject(state.brush_sel);
    }

    if (com_ok) {
        ::CoUninitialize();
    }
    result = state.result;
    result.arabic = state.arabic_ui;
#else
    (void)recent_projects;
    result.quit = true;
#endif
    return result;
}

// --- persistence -------------------------------------------------------------

std::vector<std::string> load_recent_projects() {
    std::vector<std::string> out;
    const std::string path = recent_store_path();
    if (path.empty()) {
        return out;
    }
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    // A project deleted since last session must not be offered.
    prune_missing_projects(out);
    return out;
}

void save_recent_projects(const std::vector<std::string>& recent) {
    const std::string path = recent_store_path();
    if (path.empty()) {
        return;
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return;
    }
    for (const std::string& p : recent) {
        out << p << '\n';
    }
}

// --- shell settings (language + window state) --------------------------------
// %APPDATA%/SANAD/settings.json. The recent list keeps its own file in
// LOCALAPPDATA (see above); this one is only language + window state.

#ifdef _WIN32
std::string settings_store_path() {
    std::filesystem::path p;
    char buf[MAX_PATH * 4] = {};
    const DWORD n = ::GetEnvironmentVariableA("APPDATA", buf, sizeof(buf));
    if (n != 0 && n < sizeof(buf)) {
        p = buf;
    } else {
        // A GUI process launched from a stripped environment can lack APPDATA.
        // Ask the shell for the real roaming folder instead of silently
        // falling back to English defaults and ignoring the saved language.
        PWSTR known = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &known))) {
            p = known;
            ::CoTaskMemFree(known);
        }
    }
    if (p.empty()) {
        return {};
    }
    p /= "SANAD";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    p /= "settings.json";
    return p.string();
}
#endif

LauncherSettings load_settings() {
#ifdef _WIN32
    LauncherSettings s;
    const std::string path = settings_store_path();
    if (path.empty()) {
        return s;
    }
    std::ifstream in(path);
    if (!in) {
        return s; // first run: defaults (English, maximized)
    }
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return settings_from_json(json);
#else
    return LauncherSettings{};
#endif
}

void save_settings(const LauncherSettings& s) {
#ifdef _WIN32
    const std::string path = settings_store_path();
    if (path.empty()) {
        return;
    }
    std::ofstream out(path, std::ios::trunc);
    if (out) {
        out << settings_to_json(s);
    }
#else
    (void)s;
#endif
}

void save_language(bool arabic) {
    LauncherSettings s = load_settings();
    s.arabic = arabic;
    save_settings(s);
}

// --- native pickers ----------------------------------------------------------

bool pick_folder_dialog(const std::string& title, const std::string& start_dir,
                        std::string& out_dir, void* owner) {
#ifdef _WIN32
    // Own the modal dialog explicitly: GetActiveWindow() can return null while
    // the launcher's worker thread is repainting, which used to let the picker
    // open behind the launcher (the window then looked frozen).
    HWND owner_wnd = static_cast<HWND>(owner);
    if (owner_wnd == nullptr) {
        owner_wnd = ::GetActiveWindow();
    }
    IFileDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg)))) {
        return false;
    }
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    const std::wstring wtitle = widen(title);
    dlg->SetTitle(wtitle.c_str());
    if (!start_dir.empty()) {
        IShellItem* start = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(widen(start_dir).c_str(), nullptr,
                                                    IID_PPV_ARGS(&start)))) {
            dlg->SetFolder(start);
            start->Release();
        }
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner_wnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR raw = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw != nullptr) {
                out_dir = narrow(raw);
                ok = !out_dir.empty();
                ::CoTaskMemFree(raw);
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok; // false == cancelled, which is normal
#else
    (void)title;
    (void)start_dir;
    (void)out_dir;
    (void)owner;
    return false;
#endif
}

bool pick_project_file_dialog(const std::string& title, const std::string& start_dir,
                              std::string& out_file, void* owner) {
#ifdef _WIN32
    HWND owner_wnd = static_cast<HWND>(owner);
    if (owner_wnd == nullptr) {
        owner_wnd = ::GetActiveWindow();
    }
    IFileDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg)))) {
        return false;
    }
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    }
    const COMDLG_FILTERSPEC spec[] = {{L"SANAD project", L"*.nfproj"},
                                      {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, spec);
    dlg->SetFileTypeIndex(1);
    const std::wstring wtitle = widen(title);
    dlg->SetTitle(wtitle.c_str());
    if (!start_dir.empty()) {
        IShellItem* start = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(widen(start_dir).c_str(), nullptr,
                                                    IID_PPV_ARGS(&start)))) {
            dlg->SetFolder(start);
            start->Release();
        }
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner_wnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR raw = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw != nullptr) {
                out_file = narrow(raw);
                ok = !out_file.empty();
                ::CoTaskMemFree(raw);
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
#else
    (void)title;
    (void)start_dir;
    (void)out_file;
    (void)owner;
    return false;
#endif
}

} // namespace nf::editor
