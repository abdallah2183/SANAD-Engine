// FileSystemPanel.cpp — Godot-style FileSystem dock for NOVAForge.
//
// Layout mirrors the reference (second screenshot in the request):
//   [FileSystem] [History]            <- top tabs
//   [<] [>] [path breadcrumb...] [refresh] [grid/list]   <- nav bar
//   [Filter Files...........] [Type: All]                <- filter row
//   +----------------+-------------------------------+
//   | ★ Favorites    |  folder content (icons)       |
//   | res://         |  ...                          |
//   |  Meshes        |                               |
//   |  Scenes        |                               |
//   +----------------+-------------------------------+
//   status: N folders, M files
//
// Icons are drawn with ImDrawList (never emoji/unicode symbols): the old
// toolbar proved symbol glyphs rasterise as tofu with the UI font.
// File/folder names are Latin logical paths shown with TextUnformatted —
// never through AV()/shaping (shaping is for UI labels only).

#define _CRT_SECURE_NO_WARNINGS
#include <NF/Editor/FileSystemPanel.hpp>
#include <NF/Editor/UiText.hpp>
#include <NF/Editor/UiTheme.hpp>
#include <NF/Editor/UiRenderer.hpp>

#include <NF/UI/Localization.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nf::editor {

namespace {

bool fs_ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void push_info(ConsoleBuffer& console, const std::string& what) {
    console.push(LogMessage{LogLevel::Info, LogCategory::Editor, what,
                            std::chrono::system_clock::now(), __FILE__, __LINE__});
}

void push_error(ConsoleBuffer& console, const std::string& what, const std::string& err) {
    console.push(LogMessage{LogLevel::Error, LogCategory::Editor, what + ": " + err,
                            std::chrono::system_clock::now(), __FILE__, __LINE__});
}

// --- File icons (ImDrawList, DPI-independent) -------------------------------

ImU32 fs_icon_color(AssetIconKind kind) {
    switch (kind) {
        case AssetIconKind::Folder: return IM_COL32(232, 184, 92, 255);   // amber
        case AssetIconKind::Mesh: return IM_COL32(96, 150, 235, 255);     // blue cube
        case AssetIconKind::Scene: return IM_COL32(112, 200, 124, 255);   // green
        case AssetIconKind::Prefab: return IM_COL32(172, 124, 232, 255);  // purple
        case AssetIconKind::Material: return IM_COL32(235, 130, 90, 255); // orange-red
        case AssetIconKind::Texture: return IM_COL32(84, 200, 190, 255);  // teal
        case AssetIconKind::Script: return IM_COL32(140, 190, 140, 255);  // soft green
        case AssetIconKind::Shader: return IM_COL32(225, 130, 205, 255);  // pink
        case AssetIconKind::Audio: return IM_COL32(200, 210, 110, 255);   // lime
        case AssetIconKind::Unknown: return IM_COL32(150, 155, 165, 255); // gray
    }
    return IM_COL32(150, 155, 165, 255);
}

void draw_fs_icon(ImDrawList* dl, ImVec2 c, float s, AssetIconKind kind) {
    const ImU32 col = fs_icon_color(kind);
    const float h = s * 0.5f;
    const float t = (s * 0.09f > 1.2f) ? s * 0.09f : 1.2f;
    switch (kind) {
        case AssetIconKind::Folder: {
            // Tab + body.
            const ImVec2 tab0(c.x - h, c.y - h * 0.7f);
            const ImVec2 tab1(c.x - h * 0.1f, c.y - h * 0.7f);
            dl->AddRectFilled(ImVec2(tab0.x, tab0.y - h * 0.25f),
                              ImVec2(tab1.x, tab1.y + 1.0f), col, 1.0f);
            dl->AddRectFilled(ImVec2(c.x - h, c.y - h * 0.55f),
                              ImVec2(c.x + h, c.y + h * 0.75f), col, 2.0f);
            // Darker inner line for depth.
            dl->AddRect(ImVec2(c.x - h, c.y - h * 0.55f),
                        ImVec2(c.x + h, c.y + h * 0.75f),
                        IM_COL32(60, 45, 20, 255), 2.0f, 0, t * 0.7f);
            break;
        }
        case AssetIconKind::Mesh: {
            const ImVec2 top(c.x, c.y - h);
            const ImVec2 right(c.x + h, c.y - h * 0.4f);
            const ImVec2 mid(c.x, c.y + h * 0.15f);
            const ImVec2 left(c.x - h, c.y - h * 0.4f);
            const ImVec2 bottom(c.x, c.y + h);
            dl->AddQuad(top, right, mid, left, col, t);
            dl->AddQuad(left, mid, bottom, ImVec2(c.x - h, c.y + h * 0.45f), col, t);
            dl->AddQuad(mid, right, ImVec2(c.x + h, c.y + h * 0.45f), bottom, col, t);
            break;
        }
        case AssetIconKind::Scene: {
            // Viewport frame + play triangle.
            dl->AddRect(ImVec2(c.x - h, c.y - h * 0.6f), ImVec2(c.x + h, c.y + h * 0.7f),
                        col, 2.0f, 0, t);
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.3f, c.y - h * 0.3f),
                                  ImVec2(c.x - h * 0.3f, c.y + h * 0.4f),
                                  ImVec2(c.x + h * 0.45f, c.y + h * 0.05f), col);
            break;
        }
        case AssetIconKind::Prefab: {
            // Cube + small link square at corner.
            const float q = h * 0.75f;
            dl->AddRect(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q * 0.5f, c.y + q * 0.5f),
                        col, 1.0f, 0, t);
            dl->AddRectFilled(ImVec2(c.x + q * 0.1f, c.y + q * 0.1f),
                              ImVec2(c.x + h, c.y + h), col, 1.0f);
            break;
        }
        case AssetIconKind::Material: {
            dl->AddCircleFilled(c, h * 0.8f, col, 20);
            dl->AddCircle(c, h * 0.8f, IM_COL32(255, 255, 255, 90), 20, t * 0.6f);
            // Specular dot.
            dl->AddCircleFilled(ImVec2(c.x - h * 0.25f, c.y - h * 0.3f), h * 0.18f,
                                IM_COL32(255, 255, 255, 200), 12);
            break;
        }
        case AssetIconKind::Texture: {
            dl->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col, 1.5f, 0,
                        t);
            // Mountain + sun inside.
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.7f, c.y + h * 0.6f),
                                  ImVec2(c.x - h * 0.05f, c.y - h * 0.25f),
                                  ImVec2(c.x + h * 0.6f, c.y + h * 0.6f),
                                  IM_COL32(40, 60, 70, 255));
            dl->AddCircleFilled(ImVec2(c.x + h * 0.4f, c.y - h * 0.4f), h * 0.18f,
                                IM_COL32(255, 240, 180, 255), 10);
            break;
        }
        case AssetIconKind::Script: {
            // Document + code lines.
            const float w = h * 0.7f;
            dl->AddRect(ImVec2(c.x - w, c.y - h), ImVec2(c.x + w, c.y + h), col, 1.0f,
                        0, t);
            for (int i = 0; i < 3; ++i) {
                const float y = c.y - h * 0.35f + static_cast<float>(i) * h * 0.4f;
                const float x1 = c.x - w * 0.6f + (i == 1 ? w * 0.3f : 0.0f);
                dl->AddLine(ImVec2(x1, y), ImVec2(c.x + w * 0.6f, y), col, t * 0.8f);
            }
            break;
        }
        case AssetIconKind::Shader: {
            // Diamond / nodes.
            dl->AddQuad(ImVec2(c.x, c.y - h), ImVec2(c.x + h * 0.7f, c.y),
                        ImVec2(c.x, c.y + h), ImVec2(c.x - h * 0.7f, c.y), col, t);
            dl->AddCircleFilled(c, h * 0.18f, col, 10);
            break;
        }
        case AssetIconKind::Audio: {
            // Speaker + waves.
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.7f, c.y - h * 0.3f),
                                  ImVec2(c.x - h * 0.1f, c.y - h * 0.3f),
                                  ImVec2(c.x - h * 0.1f, c.y + h * 0.3f), col);
            dl->AddTriangleFilled(ImVec2(c.x - h * 0.7f, c.y + h * 0.3f),
                                  ImVec2(c.x - h * 0.7f, c.y - h * 0.3f),
                                  ImVec2(c.x - h * 0.1f, c.y + h * 0.3f), col);
            dl->AddRect(ImVec2(c.x - h * 0.1f, c.y - h * 0.3f),
                        ImVec2(c.x + h * 0.1f, c.y + h * 0.3f), col, 0.0f, 0, t);
            dl->PathArcTo(ImVec2(c.x + h * 0.1f, c.y), h * 0.45f, -0.9f, 0.9f, 10);
            dl->PathStroke(col, 0, t);
            dl->PathArcTo(ImVec2(c.x + h * 0.1f, c.y), h * 0.75f, -0.9f, 0.9f, 12);
            dl->PathStroke(col, 0, t);
            break;
        }
        case AssetIconKind::Unknown: {
            dl->AddRect(ImVec2(c.x - h * 0.6f, c.y - h), ImVec2(c.x + h * 0.6f, c.y + h),
                        col, 1.0f, 0, t);
            dl->AddLine(ImVec2(c.x - h * 0.6f, c.y), ImVec2(c.x + h * 0.6f, c.y), col,
                        t * 0.7f);
            break;
        }
    }
}

const char* icon_type_tag(AssetIconKind kind) {
    switch (kind) {
        case AssetIconKind::Folder: return "Folder";
        case AssetIconKind::Mesh: return "Mesh";
        case AssetIconKind::Scene: return "Scene";
        case AssetIconKind::Prefab: return "Prefab";
        case AssetIconKind::Material: return "Material";
        case AssetIconKind::Texture: return "Texture";
        case AssetIconKind::Script: return "Script";
        case AssetIconKind::Shader: return "Shader";
        case AssetIconKind::Audio: return "Audio";
        case AssetIconKind::Unknown: return "File";
    }
    return "File";
}

// Small inline icon widget: reserves size x size, draws centered.
void fs_icon_widget(AssetIconKind kind, float size = 18.0f) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    draw_fs_icon(ImGui::GetWindowDrawList(),
                 ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.8f, kind);
}

// Trims `text` with a trailing ellipsis so it fits `max_w` pixels.
// Grid cells are fixed-width: without this a long file name paints over the
// neighbouring cell. The full name stays visible in the tooltip.
std::string ellipsize_to_width(const std::string& text, float max_w) {
    if (text.empty() || ImGui::CalcTextSize(text.c_str()).x <= max_w) {
        return text;
    }
    std::string out = text;
    while (out.size() > 4 &&
           ImGui::CalcTextSize((out + "...").c_str()).x > max_w) {
        out.pop_back();
    }
    if (out.size() <= 4) {
        return "...";
    }
    return out + "...";
}

// --- Rename modal state -----------------------------------------------------
//
// One slot, not a map: a rename is a single blocking interaction (the dock is
// the only thing that can start one), and a second rename while the first is
// open would have to either queue or steal focus. `open` false means no modal.
struct RenameState {
    bool open = false;
    std::string from;
    char name[128]{};
};
RenameState& rename_state() {
    static RenameState s;
    return s;
}

// --- Delete confirmation ----------------------------------------------------
//
// One slot for the same reason the rename has one: the dock is the only thing
// that opens it, and two overlapping confirms would have to queue. `folder`
// selects the wording AND the recursive flag — a folder delete takes its
// contents with it, and the prompt says so.
struct ConfirmState {
    bool open = false;
    std::string path;
    bool folder = false;
};
ConfirmState& confirm_state() {
    static ConfirmState s;
    return s;
}

void begin_rename(RenameState& s, const std::string& from) {
    s.open = true;
    s.from = from;
    // Prefill with the current file name (not the whole path): the user is
    // renaming within the folder they are standing in.
    const std::string nm = file_name_of(from);
    std::strncpy(s.name, nm.c_str(), sizeof(s.name) - 1);
    s.name[sizeof(s.name) - 1] = '\0';
}

// Builds the destination logical path: same folder, new file name.
std::string renamed_path(const std::string& from, const std::string& new_name) {
    const std::string folder = asset_folder_of(from);
    if (folder.empty()) {
        return new_name;
    }
    return folder + "/" + new_name;
}

// Returns true when a rename completed this frame (so the caller can drop a
// confirmation line). Any failure is reported to the console and the modal is
// closed either way: keeping it open on an error with no message next to it is
// worse than closing with the error in the console.
bool finish_rename(EditorApp& app, RenameState& s) {
    const std::string typed(s.name);
    s.open = false;
    // An emptied box is a cancel, not a rename to "". Refusing here rather than
    // in EditorApp keeps the modal responsive: the user gets nothing at all,
    // which reads as "the field was empty, I did not mean that".
    if (typed.empty()) {
        return false;
    }
    const std::string to = renamed_path(s.from, typed);
    if (to == s.from) {
        return false; // retyped the same name: nothing to do, nothing to report
    }
    std::string err;
    if (!app.rename_asset(s.from, to, err)) {
        push_error(app.console(), AV("rename_failed"), err);
        return false;
    }
    push_info(app.console(), AVF("renamed_fmt", to.c_str()));
    return true;
}

// Returns true when a delete completed. `folder` picks the recursive call and
// the wording of the confirm; both were captured when the confirm opened, so a
// path that changed underneath cannot flip a file delete into a tree delete.
bool finish_delete(EditorApp& app, ConfirmState& c) {
    std::string err;
    if (!app.delete_asset(c.path, c.folder, err)) {
        push_error(app.console(), AV("delete_asset"), err);
        return false;
    }
    push_info(app.console(), AVF("deleted_fmt", c.path.c_str()));
    return true;
}

// Defined at the end of the file, next to each other so the two modal bodies sit
// side by side. Declared here because the panel body that opens them is above.
void draw_rename_modal(EditorApp& app);
void draw_delete_confirm(EditorApp& app);

// Star for favorites: filled amber when fav, outline otherwise.
void draw_star(ImDrawList* dl, ImVec2 c, float s, bool filled) {
    // 5-point star from 10 vertices.
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        const float r = (i % 2 == 0) ? s * 0.5f : s * 0.22f;
        const float a = -3.14159265f * 0.5f + static_cast<float>(i) * 3.14159265f / 5.0f;
        pts[i] = ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r);
    }
    if (filled) {
        dl->AddConvexPolyFilled(pts, 10, IM_COL32(240, 196, 90, 255));
    } else {
        dl->AddPolyline(pts, 10, IM_COL32(150, 150, 155, 255), ImDrawFlags_Closed, 1.4f);
    }
}

bool star_button(const char* id, bool filled, const char* tooltip, float size = 18.0f) {
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // InvisibleButton's return IS the click (release inside bounds): the old
    // IsItemActivated()+IsItemHovered() pair fired on press-down instead and
    // missed keyboard activation entirely.
    const bool clicked = ImGui::InvisibleButton("##star", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered) {
        dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size),
                          ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    }
    draw_star(dl, ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size, filled);
    if (hovered && tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::PopID();
    return clicked;
}

// Dispatch double-click exactly like the old panel (kept in one place).
void dispatch_open(EditorApp& app, UiIntents& intents, const AssetEntry& e) {
    std::string info;
    const AssetOpenAction act = classify_double_click(e, info);
    if (act == AssetOpenAction::OpenScene) {
        intents.open_scene_dialog_confirm = true;
        intents.open_scene_path = info;
    } else if (act == AssetOpenAction::ShowMeshInfo) {
        app.console().push(LogMessage{LogLevel::Info, LogCategory::Editor, info,
                                      std::chrono::system_clock::now(), __FILE__, __LINE__});
    } else if (act == AssetOpenAction::ShowTextureInfo) {
        app.console().push(LogMessage{LogLevel::Info, LogCategory::Editor,
                                      "Texture " + info +
                                          " — assign it from an entity's Material section.",
                                      std::chrono::system_clock::now(), __FILE__, __LINE__});
    } else if (act == AssetOpenAction::ShowMaterialInfo) {
        std::string detail = info;
        if (app.runtime() != nullptr) {
            rendering::PBRMaterialParams mp{};
            app.runtime()->material_params(info, mp);
            char buf[160];
            std::snprintf(buf, sizeof(buf), " base=(%.2f,%.2f,%.2f) metallic=%.2f roughness=%.2f",
                          static_cast<double>(mp.base_color[0]),
                          static_cast<double>(mp.base_color[1]),
                          static_cast<double>(mp.base_color[2]),
                          static_cast<double>(mp.metallic), static_cast<double>(mp.roughness));
            detail += buf;
        }
        app.console().push(LogMessage{LogLevel::Info, LogCategory::Editor, detail,
                                      std::chrono::system_clock::now(), __FILE__, __LINE__});
    } else if (act == AssetOpenAction::OpenInIde) {
        // Double-clicked a script: same path as the context-menu button, with
        // the result reported to the console either way (never silent).
        std::string kind, err;
        if (!app.open_asset_in_ide(info, kind, err)) {
            push_error(app.console(), "Open in IDE failed", err);
        } else {
            push_info(app.console(), std::string("Opened in ") + kind + ": " + info);
        }
    }
}

// Recursive folder tree. Ancestors of the current folder stay open so the
// selection is always visible; click navigates, arrows expand/collapse.
void draw_folder_tree(EditorApp& app, const std::vector<std::string>& all_folders,
                      const std::string& folder, const std::string& nav_base, int depth) {
    if (depth > 12) {
        return; // pathological nesting guard
    }
    const std::vector<std::string> subs = subfolders_in_folder(all_folders, folder);
    for (const std::string& sub : subs) {
        ImGui::PushID(sub.c_str());
        const bool is_current = (app.browser().current_folder == sub);
        const bool is_ancestor =
            !app.browser().current_folder.empty() &&
            app.browser().current_folder.rfind(sub, 0) == 0 &&
            app.browser().current_folder.size() > sub.size();
        const bool has_kids = !subfolders_in_folder(all_folders, sub).empty();
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (is_current) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (!has_kids) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        if (is_ancestor) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        }
        const std::string name = folder_display_name(sub);
        const bool open = ImGui::TreeNodeEx(name.c_str(), flags);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
            browser_navigate_to(app.browser(), sub);
        }
        // Folder context: favorite toggle + copy path.
        if (ImGui::BeginPopupContextItem("##folderctx")) {
            const bool fav = is_favorite(app.browser(), sub);
            if (ImGui::MenuItem(fav ? AV("remove_favorite").c_str()
                                    : AV("add_favorite").c_str())) {
                toggle_favorite(app.browser(), sub);
            }
            if (ImGui::MenuItem(AV("show_in_folder").c_str())) {
                ImGui::SetClipboardText(sub.c_str());
                push_info(app.console(), "Path copied: " + sub);
            }
            ImGui::EndPopup();
        }
        if (open && has_kids) {
            draw_folder_tree(app, all_folders, sub, nav_base, depth + 1);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

} // namespace

void filesystem_panel(EditorApp& app, UiIntents& intents) {
    AssetBrowserState& bs = app.browser();
    const std::string nav_base =
        bs.current_folder.empty()
            ? (bs.browser_root == 1 ? std::string("project://") : std::string("content://"))
            : bs.current_folder;

    // --- Width and height budget --------------------------------------------
    //
    // Every row below used to be laid out for a wide bottom dock: a fixed 230px
    // favourites column, PushItemWidth(-220) on the filter, the breadcrumb and
    // six buttons on one line. Dropped into a narrow column (or any dock the
    // user has dragged small) the fixed column ate the file list and the
    // negative item width went to zero, which reads as "the panel overlaps
    // itself". So: measure once, then choose a layout that fits.
    //
    // Height matters just as much as width: this panel now lives under the
    // outliner in the left column, where it is routinely ~250px tall. Six rows
    // of chrome would leave the file list nothing, so the optional rows
    // (root combo, import tools, counts) are dropped in that order as the dock
    // gets shorter — the browser itself is the one thing that must never shrink
    // to nothing.
    const ImVec2 panel_avail = ImGui::GetContentRegionAvail();
    const float panel_w = panel_avail.x;
    const float panel_h = panel_avail.y;
    const bool compact = panel_w < 430.0f; // narrow column: stack, drop the crumb
    const float row_h = ImGui::GetFrameHeight();
    const bool show_root_combo = panel_h > 320.0f;
    const bool show_import = panel_h > 300.0f;
    const bool show_status = panel_h > 215.0f;
    // The view toggles prefer the tab row (it is otherwise half empty) and fall
    // back to the nav row, then to a row of their own. Set below.
    bool views_placed = false;
    const ImGuiStyle& st = ImGui::GetStyle();
    // A SmallButton is exactly its text plus FramePadding on each side — ImGui
    // adds no inner padding — so this is the real width, not an estimate. The
    // old +8 fudge is why a button that "should" have fit still clipped.
    const auto small_btn_w = [&st](const std::string& label) {
        return ImGui::CalcTextSize(label.c_str()).x + st.FramePadding.x * 2.0f;
    };
    const std::string list_l = AV("view_list");
    const std::string grid_l = AV("view_grid");
    const std::string refresh_l = AV("refresh");

    // --- Top tabs: Files | History -----------------------------------------
    //
    // "Files" rather than "FileSystem": the dock tab above already carries the
    // panel's name, and repeating it made two identical labels sit one above the
    // other — indistinguishable from a broken paint. These are a VIEW switcher.
    //
    // The list/grid toggle shares this row when it fits: the row is otherwise
    // half empty, and a row saved is a row the browser gets back.
    const auto draw_view_toggle = [&]() {
        const bool is_grid = (bs.view_mode == 1);
        if (!is_grid) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::SmallButton(list_l.c_str())) {
            bs.view_mode = 0;
        }
        if (!is_grid) {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        if (is_grid) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::SmallButton(grid_l.c_str())) {
            bs.view_mode = 1;
        }
        if (is_grid) {
            ImGui::PopStyleColor();
        }
    };
    {
        const bool fs_active = (bs.file_tab == 0);
        const bool hist_active = (bs.file_tab == 1);
        if (!fs_active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        }
        if (ImGui::Button(AV("files").c_str())) {
            bs.file_tab = 0;
        }
        if (!fs_active) {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        if (!hist_active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        }
        // History count = back + current + forward.
        const size_t hist_n = bs.back_stack.size() + bs.forward_stack.size() + 1;
        const std::string hist_label =
            AV("history") + " (" + std::to_string(hist_n) + ")";
        if (ImGui::Button(hist_label.c_str())) {
            bs.file_tab = 1;
        }
        if (!hist_active) {
            ImGui::PopStyleColor();
        }
        // Measure AFTER SameLine(). Before it ImGui has already advanced the
        // cursor onto the next line, so GetContentRegionAvail() reports the FULL
        // width rather than what is left on this row — measuring first is what
        // placed the toggle past the right edge, where the window clip rect
        // swallowed it. A control that is drawn but clipped is indistinguishable
        // from a missing control.
        ImGui::SameLine();
        const float tab_slack = ImGui::GetContentRegionAvail().x;
        const float tab_need = small_btn_w(list_l) + small_btn_w(grid_l) + st.ItemSpacing.x;
        if (tab_slack >= tab_need + 8.0f) {
            views_placed = true;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + tab_slack - tab_need);
            draw_view_toggle();
        } else {
            ImGui::NewLine(); // undo the SameLine so the next row starts clean
        }
    }
    ImGui::Separator();

    if (bs.file_tab == 1) {
        // --- History tab: every visited folder, newest last -----------------
        ImGui::TextDisabled("%s", AV("history").c_str());
        if (ImGui::BeginChild("##fshistory", ImVec2(0, 0), true)) {
            // Row identity is the POSITION in the list, never the folder path.
            // A path visited twice (A -> B -> A) is legitimate history, and the
            // path-as-ID made those two rows submit the same ID — which is the
            // "2 visible items with conflicting ID" popup the lead hit here.
            int hist_index = 0;
            auto hist_row = [&](const std::string& folder, bool is_current) {
                if (folder.empty()) {
                    return;
                }
                ImGui::PushID(hist_index++);
                fs_icon_widget(AssetIconKind::Folder, 16.0f);
                ImGui::SameLine();
                const bool sel = is_current;
                if (ImGui::Selectable(folder.c_str(), sel)) {
                    browser_navigate_to(bs, folder);
                    bs.file_tab = 0;
                }
                ImGui::PopID();
            };
            for (const std::string& f : bs.back_stack) {
                hist_row(f, false);
            }
            hist_row(nav_base, true);
            for (auto it = bs.forward_stack.rbegin(); it != bs.forward_stack.rend(); ++it) {
                hist_row(*it, false);
            }
        }
        ImGui::EndChild();
        return;
    }

    // --- Nav bar: back / forward / up / breadcrumb / refresh / view --------
    {
        const bool can_back = browser_can_go_back(bs);
        const bool can_fwd = browser_can_go_forward(bs);
        if (!can_back) {
            ImGui::BeginDisabled();
        }
        if (ImGui::ArrowButton("##fsback", ImGuiDir_Left)) {
            if (browser_go_back(bs)) {
                app.invalidate_browser();
            }
        }
        if (!can_back) {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", AV("back").c_str());
        }
        ImGui::SameLine();
        if (!can_fwd) {
            ImGui::BeginDisabled();
        }
        if (ImGui::ArrowButton("##fsfwd", ImGuiDir_Right)) {
            if (browser_go_forward(bs)) {
                app.invalidate_browser();
            }
        }
        if (!can_fwd) {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", AV("forward").c_str());
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##fsup", ImGuiDir_Up)) {
            // Up: Godot shows the parent path; keep the old Up behaviour.
            if (!is_root_folder(nav_base)) {
                browser_navigate_to(bs, parent_folder_of(nav_base));
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", AV("browser_up").c_str());
        }
        // Everything after the arrows is placed against a real x-budget: a
        // control that does not fit WRAPS to the next row instead of being drawn
        // half off the panel. A clipped button is indistinguishable from a
        // broken layout, which is the whole complaint this answers.
        const float nav_budget = ImGui::GetContentRegionAvail().x;
        const float arrow_w = ImGui::GetFrameHeight() * 3.0f + st.ItemSpacing.x * 2.0f;
        const float refresh_w = small_btn_w(refresh_l) + st.ItemSpacing.x;
        const float views_w =
            small_btn_w(list_l) + small_btn_w(grid_l) + st.ItemSpacing.x * 2.0f;
        // Crumb: the whole path when it fits, ellipsised to the room that is
        // left when it does not. The path gets first claim on the row — knowing
        // where you are is the one thing this row is for, and a button pushed
        // to the second row costs nothing while a missing path costs context.
        const float crumb_room = nav_budget - arrow_w - st.ItemSpacing.x;
        const float crumb_need = small_btn_w(nav_base) + st.ItemSpacing.x;
        float crumb_w = 0.0f;
        std::string crumb_label;
        bool crumb_ellipsised = false;
        if (crumb_room > 70.0f) {
            crumb_w = (crumb_need < crumb_room) ? crumb_need : crumb_room;
            crumb_label = (crumb_need < crumb_room)
                              ? nav_base
                              : ellipsize_to_width(nav_base, crumb_w - st.FramePadding.x * 2.0f);
            crumb_ellipsised = (crumb_need >= crumb_room);
        }
        // Refresh keeps the nav row when it fits there; otherwise it moves down
        // to share the toggle's row — a row holding two buttons has room for
        // three, and dropping refresh would cost the hot-reload workflow.
        const bool refresh_on_nav =
            nav_budget - arrow_w - crumb_w - refresh_w >= 0.0f && nav_budget > 300.0f;
        const bool views_on_nav = !views_placed &&
                                  nav_budget - arrow_w - crumb_w -
                                          (refresh_on_nav ? refresh_w : 0.0f) - views_w >= 0.0f;
        // Views that fit nowhere on the nav row get a row of their own: dropping
        // the list/grid switch entirely would be a worse answer than one row.
        const bool second_row = !views_placed && !views_on_nav;

        ImGui::SameLine();
        if (crumb_w > 0.0f) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            if (ImGui::SmallButton(crumb_label.c_str())) {
                ImGui::SetClipboardText(nav_base.c_str());
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", crumb_ellipsised ? nav_base.c_str()
                                                         : AV("show_in_folder").c_str());
            }
        }
        if (refresh_on_nav) {
            ImGui::SameLine();
            if (ImGui::SmallButton(refresh_l.c_str())) {
                app.invalidate_browser();
            }
        }
        if (views_on_nav) {
            ImGui::SameLine();
            draw_view_toggle();
        } else if (second_row) {
            ImGui::NewLine();
            draw_view_toggle();
            ImGui::SameLine();
            if (ImGui::SmallButton(refresh_l.c_str())) {
                app.invalidate_browser();
            }
        }
        // Root combo (Content / Project): right-aligned when the row has slack,
        // otherwise on a row of its own at full width. Dropped entirely when the
        // dock is short — it is a once-a-session switch and the browser needs the
        // row more than it does.
        const float combo_w = 150.0f;
        float slack = 0.0f;
        if (show_root_combo) {
            ImGui::SameLine();
            slack = ImGui::GetContentRegionAvail().x;
        }
        float root_w = 0.0f;
        if (show_root_combo) {
            if (slack > combo_w + 10.0f) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + slack - combo_w);
                root_w = combo_w;
            } else {
                ImGui::NewLine();
                root_w = ImGui::GetContentRegionAvail().x;
            }
        }
        if (root_w > 90.0f) {
            ImGui::PushItemWidth(root_w);
            const std::string root_items[2] = {AV("browser_content"), AV("browser_project")};
            const char* root_names[] = {root_items[0].c_str(), root_items[1].c_str()};
            int root_idx = (bs.browser_root == 1) ? 1 : 0;
            if (ImGui::Combo("##fsroot", &root_idx, root_names, 2)) {
                bs.browser_root = root_idx;
                bs.current_folder.clear();
                bs.back_stack.clear();
                bs.forward_stack.clear();
                app.invalidate_browser();
            }
            ImGui::PopItemWidth();
        }
    }

    // --- Filter row ---------------------------------------------------------
    {
        char filter[128]{};
        std::strncpy(filter, bs.filter_text.c_str(), sizeof(filter) - 1);
        const std::string hint = AV("filter_files");
        // A fixed -220 assumed a wide dock; on a narrow panel the negative width
        // collapses the field to nothing, which is exactly the "squashed" look.
        // The type combo keeps a readable floor and the field takes the rest, so
        // both stay on one row at any width.
        const float type_w = compact ? 104.0f : 190.0f;
        ImGui::PushItemWidth(-type_w - 10.0f);
        if (ImGui::InputTextWithHint("##fsfilter", hint.c_str(), filter, sizeof(filter))) {
            bs.filter_text = filter;
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        const std::string type_items[7] = {AV("asset_type_all"), AV("asset_type_mesh"),
                                           AV("asset_type_texture"), AV("asset_type_material"),
                                           AV("asset_type_shader"), AV("asset_type_scene"),
                                           AV("asset_type_script")};
        const char* type_names[] = {type_items[0].c_str(), type_items[1].c_str(),
                                    type_items[2].c_str(), type_items[3].c_str(),
                                    type_items[4].c_str(), type_items[5].c_str(),
                                    type_items[6].c_str()};
        const int type_values[] = {-1, static_cast<int>(assets::AssetType::Mesh),
                                   static_cast<int>(assets::AssetType::Texture),
                                   static_cast<int>(assets::AssetType::Material),
                                   static_cast<int>(assets::AssetType::Shader),
                                   static_cast<int>(assets::AssetType::Scene),
                                   static_cast<int>(assets::AssetType::Script)};
        int combo_idx = 0;
        for (size_t i = 1; i < 7; ++i) {
            if (bs.filter_type == type_values[i]) {
                combo_idx = static_cast<int>(i);
                break;
            }
        }
        ImGui::PushItemWidth(type_w);
        if (ImGui::Combo("##fstype", &combo_idx, type_names, 7)) {
            bs.filter_type = type_values[combo_idx];
        }
        ImGui::PopItemWidth();
    }
    ImGui::Separator();

    // --- Data ---------------------------------------------------------------
    const std::vector<AssetEntry> all_entries = app.browser_entries();
    const bool searching = !bs.filter_text.empty();
    const std::vector<std::string> all_folders = asset_folders(all_entries);
    std::vector<std::string> nav_subfolders;
    std::vector<AssetEntry> nav_files;
    std::vector<AssetEntry> search_entries;
    if (searching) {
        search_entries = all_entries;
    } else {
        nav_subfolders = subfolders_in_folder(all_folders, nav_base);
        nav_files = assets_in_folder(all_entries, nav_base);
    }

    // --- Split: favourites/tree | file list --------------------------------
    //
    // Side by side when there is width for two readable columns, stacked when
    // there is not. The fixed 230px column this replaces WAS the bug: in a
    // narrow dock it took the whole panel and left the file list a few pixels
    // wide, which reads as the two panes having been painted on top of each
    // other.
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float avail_h = ImGui::GetContentRegionAvail().y - row_h -
                          (show_status ? row_h * 0.8f : 0.0f) -
                          (show_import ? row_h : 0.0f) - 12.0f;
    const float body_h = (avail_h > 70.0f) ? avail_h : 70.0f;
    const bool stacked = compact || avail_w < 430.0f;
    float left_w = 0.0f;
    float left_h = body_h;
    float right_h = body_h;
    if (stacked) {
        // Favourites + folder tree get a third of the body: enough for a few
        // starred folders and a scrollable tree, while leaving the file list the
        // larger share — the list is what the panel is for.
        left_h = body_h * 0.34f;
        right_h = body_h - left_h - ImGui::GetStyle().ItemSpacing.y;
        if (right_h < 54.0f) {
            right_h = 54.0f;
        }
    } else {
        left_w = avail_w * 0.34f;
        if (left_w < 150.0f) {
            left_w = 150.0f;
        }
        if (left_w > 250.0f) {
            left_w = 250.0f;
        }
    }
    if (ImGui::BeginChild("##fsleft", ImVec2(left_w, left_h), true)) {
        // Favorites.
        ImGui::TextDisabled("%s", AV("favorites").c_str());
        // Star toggle for current folder.
        {
            const bool fav = is_favorite(bs, nav_base);
            ImGui::SameLine();
            if (star_button("##favcur", fav,
                            fav ? AV("remove_favorite").c_str() : AV("add_favorite").c_str(),
                            16.0f)) {
                toggle_favorite(bs, nav_base);
            }
        }
        if (bs.favorites.empty()) {
            ImGui::TextDisabled("  -");
        } else {
            // Index-keyed like the history rows: the path is what the row SHOWS,
            // not what identifies it. A persisted favourites list with a repeat
            // (or a future merge) would otherwise submit two items with one ID.
            int fav_index = 0;
            for (const std::string& fav : bs.favorites) {
                ImGui::PushID(fav_index++);
                const bool is_cur = (fav == nav_base);
                if (ImGui::Selectable(
                        (std::string("* ") + folder_display_name(fav)).c_str(), is_cur)) {
                    browser_navigate_to(bs, fav);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", fav.c_str());
                }
                if (ImGui::BeginPopupContextItem("##favctx")) {
                    if (ImGui::MenuItem(AV("remove_favorite").c_str())) {
                        remove_favorite(bs, fav);
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", AV("folders").c_str());
        // Root row.
        {
            const std::string root = (bs.browser_root == 1) ? std::string("project://")
                                                             : std::string("content://");
            const std::string root_label = (bs.browser_root == 1) ? "res://" : root;
            const bool is_cur = (nav_base == root);
            ImGuiTreeNodeFlags rflags = ImGuiTreeNodeFlags_OpenOnArrow |
                                        ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                        ImGuiTreeNodeFlags_DefaultOpen |
                                        ImGuiTreeNodeFlags_SpanAvailWidth;
            if (is_cur) {
                rflags |= ImGuiTreeNodeFlags_Selected;
            }
            const bool rop = ImGui::TreeNodeEx(root_label.c_str(), rflags);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
                browser_navigate_to(bs, root);
            }
            if (rop) {
                draw_folder_tree(app, all_folders, root, nav_base, 0);
                ImGui::TreePop();
            }
        }
    }
    ImGui::EndChild();
    if (!stacked) {
        ImGui::SameLine();
    }
    if (ImGui::BeginChild("##fsright", ImVec2(stacked ? 0.0f : ImGui::GetContentRegionAvail().x,
                                               right_h),
                          true)) {
        if (searching) {
            // Flat search results (old behaviour, new row design).
            if (search_entries.empty()) {
                ImGui::TextDisabled("%s", AV("no_files_match").c_str());
            } else {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(search_entries.size()));
                while (clipper.Step()) {
                    for (int ei = clipper.DisplayStart; ei < clipper.DisplayEnd; ++ei) {
                        const AssetEntry& e = search_entries[static_cast<size_t>(ei)];
                        ImGui::PushID(ei);
                        const AssetIconKind ik = asset_icon_of(e);
                        const bool selected = (bs.selected_path == e.logical_path);
                        fs_icon_widget(ik, 18.0f);
                        ImGui::SameLine();
                        // Search spans folders: same-named files in different
                        // folders are identical rows without the parent hint.
                        const std::string label =
                            std::string(file_name_of(e.logical_path)) + "  [" +
                            icon_type_tag(ik) + "]  (" +
                            folder_display_name(asset_folder_of(e.logical_path)) + ")";
                        if (ImGui::Selectable(label.c_str(), selected)) {
                            bs.selected_path = e.logical_path;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", e.logical_path.c_str());
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                dispatch_open(app, intents, e);
                            }
                        }
                        if (e.type == assets::AssetType::Mesh &&
                            ImGui::BeginDragDropSource()) {
                            ImGui::SetDragDropPayload("NF_MESH", e.logical_path.c_str(),
                                                      e.logical_path.size() + 1);
                            ImGui::Text("%s", e.logical_path.c_str());
                            ImGui::EndDragDropSource();
                        }
                        ImGui::PopID();
                    }
                }
                clipper.End();
            }
        } else if (bs.view_mode == 0) {
            // --- List mode --------------------------------------------------
            if (nav_subfolders.empty() && nav_files.empty()) {
                // Wrapped: this is a full sentence and the panel may be a narrow
                // column — an unwrapped Text is cut mid-word at the edge.
                ImGui::TextWrapped("%s", AV("empty_folder_hint").c_str());
            }
            for (const std::string& folder : nav_subfolders) {
                ImGui::PushID(folder.c_str());
                const bool selected = (bs.selected_path == folder);
                fs_icon_widget(AssetIconKind::Folder, 18.0f);
                ImGui::SameLine();
                const std::string label = folder_display_name(folder);
                if (ImGui::Selectable(label.c_str(), selected,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    bs.selected_path = folder;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        browser_navigate_to(bs, folder);
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", folder.c_str());
                }
                if (ImGui::BeginPopupContextItem("##subctx")) {
                    const bool fav = is_favorite(bs, folder);
                    if (ImGui::MenuItem(fav ? AV("remove_favorite").c_str()
                                            : AV("add_favorite").c_str())) {
                        toggle_favorite(bs, folder);
                    }
                    if (ImGui::MenuItem(AV("rename_asset").c_str())) {
                        begin_rename(rename_state(), folder);
                    }
                    if (ImGui::MenuItem(AV("show_in_folder").c_str())) {
                        ImGui::SetClipboardText(folder.c_str());
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(AV("delete_folder").c_str())) {
                        // A folder is the destructive one, so the Delete opens a
                        // CONFIRM that names what is inside it. A single click
                        // that empties a tree is the one thing a file browser
                        // must never do.
                        confirm_state() = {true, folder, true};
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(nav_files.size()));
            while (clipper.Step()) {
                for (int ei = clipper.DisplayStart; ei < clipper.DisplayEnd; ++ei) {
                    const AssetEntry& e = nav_files[static_cast<size_t>(ei)];
                    ImGui::PushID(ei + 100000);
                    const AssetIconKind ik = asset_icon_of(e);
                    const bool selected = (bs.selected_path == e.logical_path);
                    // Texture thumbnail when available, icon otherwise.
                    bool drew_thumb = false;
                    if (e.type == assets::AssetType::Texture && app.preview_texture) {
                        const uintptr_t pid = app.preview_texture(e.logical_path);
                        if (pid != 0) {
                            ImGui::Image(static_cast<ImTextureID>(pid), ImVec2(20, 20));
                            drew_thumb = true;
                        }
                    }
                    if (!drew_thumb) {
                        fs_icon_widget(ik, 18.0f);
                    }
                    ImGui::SameLine();
                    const std::string label =
                        file_name_of(e.logical_path) + std::string("  [") +
                        icon_type_tag(ik) + "]";
                    if (ImGui::Selectable(label.c_str(), selected,
                                          ImGuiSelectableFlags_AllowDoubleClick)) {
                        bs.selected_path = e.logical_path;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            dispatch_open(app, intents, e);
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", e.logical_path.c_str());
                    }
                    if (ImGui::BeginPopupContextItem("##filectx")) {
                        if (e.type == assets::AssetType::Scene ||
                            fs_ends_with(e.logical_path, ".nfscene")) {
                            if (ImGui::MenuItem(AV("open_scene").c_str())) {
                                intents.open_scene_dialog_confirm = true;
                                intents.open_scene_path = e.logical_path;
                            }
                        }
                        if (ImGui::MenuItem(AV("rename_asset").c_str())) {
                            begin_rename(rename_state(), e.logical_path);
                        }
                        if (ImGui::MenuItem(AV("show_in_folder").c_str())) {
                            ImGui::SetClipboardText(e.logical_path.c_str());
                            push_info(app.console(), "Path copied: " + e.logical_path);
                        }
                        if (e.type == assets::AssetType::Script) {
                            if (ImGui::MenuItem(AV("open_in_vs").c_str())) {
                                std::string kind, err;
                                if (!app.open_asset_in_ide(e.logical_path, kind, err)) {
                                    push_error(app.console(), "Open in IDE failed", err);
                                } else {
                                    push_info(app.console(),
                                              std::string("Opened in ") + kind + ": " +
                                                  e.logical_path);
                                }
                            }
                        }
                        // Destructive verbs last, behind a separator, so a
                        // stray click on the top of the menu cannot reach them.
                        ImGui::Separator();
                        if (ImGui::MenuItem(AV("delete_asset").c_str())) {
                            confirm_state() = {true, e.logical_path, false};
                        }
                        ImGui::EndPopup();
                    }
                    // NOTE: no inline "open in IDE" button here on purpose. Every
                    // script row used to carry SameLine + SmallButton with the
                    // full "open in VS" label, which overflowed the narrow
                    // docked panel and broke the row layout. Opening is via
                    // double-click (dispatch_open) or the right-click menu above.
                    if (e.type == assets::AssetType::Mesh && ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("NF_MESH", e.logical_path.c_str(),
                                                  e.logical_path.size() + 1);
                        ImGui::Text("%s", e.logical_path.c_str());
                        ImGui::EndDragDropSource();
                    }
                    ImGui::PopID();
                }
            }
            clipper.End();
        } else {
            // --- Grid mode (icon thumbnails) --------------------------------
            if (nav_subfolders.empty() && nav_files.empty()) {
                // Wrapped: this is a full sentence and the panel may be a narrow
                // column — an unwrapped Text is cut mid-word at the edge.
                ImGui::TextWrapped("%s", AV("empty_folder_hint").c_str());
            }
            const float cell = 92.0f;
            int col = 0;
            for (const std::string& folder : nav_subfolders) {
                ImGui::PushID(folder.c_str());
                ImGui::BeginGroup();
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##cell", ImVec2(cell, cell + 20.0f));
                const bool hovered = ImGui::IsItemHovered();
                const bool selected = (bs.selected_path == folder);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (selected) {
                    dl->AddRectFilled(p0, ImVec2(p0.x + cell, p0.y + cell + 20.0f),
                                      ImGui::GetColorU32(ImGuiCol_HeaderActive), 4.0f);
                } else if (hovered) {
                    dl->AddRectFilled(p0, ImVec2(p0.x + cell, p0.y + cell + 20.0f),
                                      ImGui::GetColorU32(ImGuiCol_HeaderHovered), 4.0f);
                }
                draw_fs_icon(dl, ImVec2(p0.x + cell * 0.5f, p0.y + cell * 0.42f),
                             cell * 0.5f, AssetIconKind::Folder);
                const std::string folder_nm =
                    ellipsize_to_width(folder_display_name(folder), cell - 8.0f);
                dl->AddText(ImVec2(p0.x + 4.0f, p0.y + cell - 2.0f),
                            IM_COL32(230, 230, 235, 255), folder_nm.c_str());
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    bs.selected_path = folder;
                }
                if (ImGui::IsItemHovered() &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    browser_navigate_to(bs, folder);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", folder.c_str());
                }
                // Grid cells carry the SAME context menu as list rows. Having
                // the verbs only in one of two views of the same data means a
                // user who switches to the grid loses the ability to do
                // anything but open, and they have to know to switch back.
                if (ImGui::BeginPopupContextItem("##gcellctx")) {
                    const bool fav = is_favorite(bs, folder);
                    if (ImGui::MenuItem(fav ? AV("remove_favorite").c_str()
                                            : AV("add_favorite").c_str())) {
                        toggle_favorite(bs, folder);
                    }
                    if (ImGui::MenuItem(AV("rename_asset").c_str())) {
                        begin_rename(rename_state(), folder);
                    }
                    if (ImGui::MenuItem(AV("show_in_folder").c_str())) {
                        ImGui::SetClipboardText(folder.c_str());
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(AV("delete_folder").c_str())) {
                        confirm_state() = {true, folder, true};
                    }
                    ImGui::EndPopup();
                }
                ImGui::EndGroup();
                ImGui::PopID();
                ++col;
                if (col * (cell + 8.0f) + cell < ImGui::GetContentRegionAvail().x + cell) {
                    ImGui::SameLine();
                } else {
                    col = 0;
                }
            }
            if (col != 0) {
                ImGui::NewLine();
                col = 0;
            }
            for (size_t fi = 0; fi < nav_files.size(); ++fi) {
                const AssetEntry& e = nav_files[fi];
                ImGui::PushID(static_cast<int>(fi) + 200000);
                ImGui::BeginGroup();
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##fcell", ImVec2(cell, cell + 20.0f));
                const bool hovered = ImGui::IsItemHovered();
                const bool selected = (bs.selected_path == e.logical_path);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (selected) {
                    dl->AddRectFilled(p0, ImVec2(p0.x + cell, p0.y + cell + 20.0f),
                                      ImGui::GetColorU32(ImGuiCol_HeaderActive), 4.0f);
                } else if (hovered) {
                    dl->AddRectFilled(p0, ImVec2(p0.x + cell, p0.y + cell + 20.0f),
                                      ImGui::GetColorU32(ImGuiCol_HeaderHovered), 4.0f);
                }
                const AssetIconKind ik = asset_icon_of(e);
                bool drew_thumb = false;
                if (e.type == assets::AssetType::Texture && app.preview_texture) {
                    const uintptr_t pid = app.preview_texture(e.logical_path);
                    if (pid != 0) {
                        dl->AddImage(static_cast<ImTextureID>(pid),
                                     ImVec2(p0.x + cell * 0.2f, p0.y + cell * 0.08f),
                                     ImVec2(p0.x + cell * 0.8f, p0.y + cell * 0.68f));
                        drew_thumb = true;
                    }
                }
                if (!drew_thumb) {
                    draw_fs_icon(dl, ImVec2(p0.x + cell * 0.5f, p0.y + cell * 0.38f),
                                 cell * 0.48f, ik);
                }
                const std::string nm = file_name_of(e.logical_path);
                const std::string short_nm = ellipsize_to_width(nm, cell - 8.0f);
                const ImVec2 tsz = ImGui::CalcTextSize(short_nm.c_str());
                float tx = p0.x + (cell - tsz.x) * 0.5f;
                if (tx < p0.x + 2.0f) {
                    tx = p0.x + 2.0f;
                }
                dl->AddText(ImVec2(tx, p0.y + cell - 2.0f), IM_COL32(230, 230, 235, 255),
                            short_nm.c_str());
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    bs.selected_path = e.logical_path;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s [%s]", e.logical_path.c_str(),
                                      icon_type_tag(ik));
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        dispatch_open(app, intents, e);
                    }
                }
                if (ImGui::BeginPopupContextItem("##gfcellctx")) {
                    if (e.type == assets::AssetType::Scene ||
                        fs_ends_with(e.logical_path, ".nfscene")) {
                        if (ImGui::MenuItem(AV("open_scene").c_str())) {
                            intents.open_scene_dialog_confirm = true;
                            intents.open_scene_path = e.logical_path;
                        }
                    }
                    if (ImGui::MenuItem(AV("rename_asset").c_str())) {
                        begin_rename(rename_state(), e.logical_path);
                    }
                    if (ImGui::MenuItem(AV("show_in_folder").c_str())) {
                        ImGui::SetClipboardText(e.logical_path.c_str());
                        push_info(app.console(), "Path copied: " + e.logical_path);
                    }
                    if (e.type == assets::AssetType::Script) {
                        if (ImGui::MenuItem(AV("open_in_vs").c_str())) {
                            std::string kind, err;
                            if (!app.open_asset_in_ide(e.logical_path, kind, err)) {
                                push_error(app.console(), "Open in IDE failed", err);
                            } else {
                                push_info(app.console(),
                                          std::string("Opened in ") + kind + ": " +
                                              e.logical_path);
                            }
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(AV("delete_asset").c_str())) {
                        confirm_state() = {true, e.logical_path, false};
                    }
                    ImGui::EndPopup();
                }
                ImGui::EndGroup();
                if (e.type == assets::AssetType::Mesh && ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("NF_MESH", e.logical_path.c_str(),
                                              e.logical_path.size() + 1);
                    ImGui::Text("%s", e.logical_path.c_str());
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
                ++col;
                if (col * (cell + 8.0f) + cell < ImGui::GetContentRegionAvail().x + cell) {
                    ImGui::SameLine();
                } else {
                    col = 0;
                }
            }
        }
        // Empty-space context: New folder / script / refresh.
        if (ImGui::BeginPopupContextWindow("##fsempty", ImGuiPopupFlags_MouseButtonRight)) {
            if (ImGui::MenuItem(AV("new_folder").c_str())) {
                std::string err;
                const std::string created = nav_base + "/NewFolder";
                if (!app.create_asset_folder(created, err)) {
                    push_error(app.console(), "Create folder failed", err);
                } else {
                    browser_navigate_to(bs, created);
                    push_info(app.console(), std::string("Folder created: ") + created);
                }
            }
            if (ImGui::MenuItem(AV("new_script").c_str())) {
                std::string created, err;
                if (!app.create_unique_script_file(nav_base, "script", created, err)) {
                    push_error(app.console(), "Create script failed", err);
                } else {
                    push_info(app.console(), std::string("Script created: ") + created);
                    app.invalidate_browser();
                }
            }
            if (ImGui::MenuItem(AV("refresh").c_str())) {
                app.invalidate_browser();
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();

    // --- Status bar ----------------------------------------------------------
    if (show_status) {
        const size_t nf = searching ? 0 : nav_subfolders.size();
        const size_t nfi = searching ? search_entries.size() : nav_files.size();
        std::string status;
        if (searching) {
            status = std::to_string(nfi) + " " + ui::tr("files");
        } else {
            status = std::to_string(nf) + " " + ui::tr("folders") + "  |  " +
                     std::to_string(nfi) + " " + ui::tr("files");
        }
        // Compose in LOGICAL order (ui::tr), then shape ONCE for display. This
        // line used to feed ui::tr() straight to TextDisabled, so the Arabic
        // rendered unshaped — every letter reversed and unjoined, with the
        // digits landing in the wrong place. Same defect class as the old
        // "فأظشرلأا عوطق" toolbar label.
        ImGui::TextDisabled("%s", ui::shape_arabic(status).c_str());
        if (!bs.selected_path.empty()) {
            // The path is long and the panel may be narrow: ellipsise it to
            // whatever is left on the line rather than letting it run past the
            // panel edge (which is how a status line ends up looking like it
            // belongs to the neighbouring panel).
            const float avail = ImGui::GetContentRegionAvail().x - 28.0f; // "|  " + margin
            if (avail > 60.0f) {
                ImGui::SameLine();
                ImGui::TextDisabled("|  %s",
                                    ellipsize_to_width(bs.selected_path, avail).c_str());
            }
        }
    }

    // --- Import tools (collapsed by default to keep the dock a browser) ------
    if (show_import &&
        ImGui::CollapsingHeader((AV("import") + "##fsimport_tools").c_str())) {
        static char import_src[256]{};
        static char import_dir[128] = "content://Meshes";
        static bool import_overwrite = false;
        ImGui::InputText(AV("import_source").c_str(), import_src, sizeof(import_src));
        ImGui::InputText(AV("import_to").c_str(), import_dir, sizeof(import_dir));
        ImGui::Checkbox(AV("overwrite").c_str(), &import_overwrite);
        ImGui::SameLine();
        if (ImGui::Button((AV("import") + "##fsimport_run").c_str())) {
            size_t job = 0;
            std::string err;
            if (!app.import_file(import_src, import_dir, import_overwrite, job, err)) {
                push_error(app.console(), "Import rejected", err);
            } else {
                app.console().push(LogMessage{LogLevel::Info, LogCategory::Editor,
                                              "Import queued (#" + std::to_string(job) + ")",
                                              std::chrono::system_clock::now(), __FILE__, __LINE__});
            }
        }
        for (const ImportJob& job : app.import_queue().jobs()) {
            const char* job_state = "?";
            switch (job.state) {
                case ImportJob::State::Queued: job_state = "queued"; break;
                case ImportJob::State::Working: job_state = "working"; break;
                case ImportJob::State::Done: job_state = "done"; break;
                case ImportJob::State::Failed: job_state = "FAILED"; break;
            }
            // Wrapped: an absolute source path is easily wider than a docked
            // panel, and an unwrapped Text paints straight over the panel edge.
            ImGui::TextWrapped("[import #%zu] %s -> %s : %s", job.id, job.src_absolute.c_str(),
                               job.dst_logical.c_str(), job_state);
            if (job.state == ImportJob::State::Failed && !job.error.empty()) {
                ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "%s", job.error.c_str());
            }
        }
        if (ImGui::Button(AV("clear_imports").c_str())) {
            app.import_queue().clear_finished();
        }
        // Create-folder / create-script. On a wide dock they share one row; on
        // a narrow one each gets its own, instead of the last button hanging
        // off the edge.
        const bool import_row_wide = (panel_w > 520.0f);
        if (import_row_wide) {
            ImGui::SameLine();
        }
        static char new_folder_name[64] = "NewFolder";
        ImGui::PushItemWidth(import_row_wide ? 140.0f : ImGui::GetContentRegionAvail().x);
        ImGui::InputText(AV("folder_name").c_str(), new_folder_name, sizeof(new_folder_name));
        ImGui::PopItemWidth();
        if (import_row_wide) {
            ImGui::SameLine();
        }
        if (ImGui::Button(AV("new_folder").c_str())) {
            std::string err;
            const std::string created = nav_base + "/" + new_folder_name;
            if (!app.create_asset_folder(created, err)) {
                push_error(app.console(), "Create folder failed", err);
            } else {
                browser_navigate_to(bs, created);
                push_info(app.console(), std::string("Folder created: ") + created);
            }
        }
        if (import_row_wide) {
            ImGui::SameLine();
        }
        if (ImGui::Button(AV("new_script").c_str())) {
            std::string created, err;
            if (!app.create_unique_script_file(nav_base, "script", created, err)) {
                push_error(app.console(), "Create script failed", err);
            } else {
                push_info(app.console(), std::string("Script created: ") + created);
                app.invalidate_browser();
            }
        }
    }

    // --- Modals: rename + delete confirm ---------------------------------
    //
    // Drawn at the very end of the panel, outside the tab bodies, so they are
    // not clipped by the tab content rect and they work no matter which tab is
    // active when the user right-clicks into them. Both are driven from the
    // right-click menus above, never from a keyboard shortcut, so a modal can
    // never open while the user is typing in the filter box.
    draw_rename_modal(app);
    draw_delete_confirm(app);
}

namespace {

// Rename: a single text field prefilled with the current name. The modal's
// lifetime is governed by RenameState::open, not by BeginPopup's return value,
// so a click elsewhere in the dock closes it instead of leaving an invisible
// popup swallowing every key.
void draw_rename_modal(EditorApp& app) {
    RenameState& s = rename_state();
    if (!s.open) {
        return;
    }
    ImGui::OpenPopup("##rename_asset");
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 320.0f));
    ImGui::InputText("##renametext", s.name, sizeof(s.name));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", AV("name_no_spaces").c_str());
    }
    ImGui::Separator();
    // Cancel first: the safe action is the one a stray Enter lands on next to,
    // and a half-typed name is always a mistake, never an intention.
    if (ImGui::Button(AV("cancel").c_str(), ImVec2(100.0f, 0.0f))) {
        s.open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(AV("rename_asset").c_str(), ImVec2(100.0f, 0.0f))) {
        (void)finish_rename(app, s);
    }
    // Enter commits, Escape abandons. Checked after the widgets so the field
    // gets first refusal on the key, as a text field should.
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        (void)finish_rename(app, s);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        s.open = false;
    }
    ImGui::EndPopup();
}

// Delete confirm: names the target, states the consequence, and colours Delete
// with the danger role so the irreversible action is the one that reads as
// irreversible. Cancel sits first, so it is what a stray Enter lands on.
void draw_delete_confirm(EditorApp& app) {
    ConfirmState& c = confirm_state();
    if (!c.open) {
        return;
    }
    ImGui::OpenPopup("##delete_confirm");
    const std::string shown = file_name_of(c.path);
    // Wrapped: a long name plus a full sentence will not fit one line in the
    // popup, and a clipped "Delete 'Environm…" is not a confirmation.
    if (c.folder) {
        ImGui::TextWrapped("%s", AVF("delete_confirm_folder", shown.c_str()).c_str());
    } else {
        ImGui::TextWrapped("%s", AVF("delete_confirm", shown.c_str()).c_str());
    }
    ImGui::Spacing();
    if (ImGui::Button(AV("cancel").c_str(), ImVec2(100.0f, 0.0f))) {
        c.open = false;
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, theme::danger());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::danger());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::danger());
    if (ImGui::Button(AV("delete_confirm_title").c_str(), ImVec2(100.0f, 0.0f))) {
        c.open = false;
        (void)finish_delete(app, c);
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        c.open = false;
    }
    ImGui::EndPopup();
}

} // namespace

} // namespace nf::editor
