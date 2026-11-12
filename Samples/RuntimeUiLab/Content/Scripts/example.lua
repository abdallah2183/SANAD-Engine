-- RuntimeUiLab: pages (main / settings / demo / profile) over one script entity.
local page = "main"
local charge, load, paused = 65, 40, false
local show_hint = true
local spin_speed = 1.0
local mode_index = 1
local mode_count = 0
local spin_t = 0

local modes = { "Orbit", "Pulse", "Wave" }

local function anchor(anchor, w, h, margin)
    return nf.ui.anchor_rect(anchor, w, h, margin or 0.025, 300, 220)
end

local function panel_rect()
    local x, y, w, h = anchor("topleft", 0.42, 0.88, 0.025)
    return {x = x, y = y, w = w, h = h}
end

-- Profile & Settings state (persisted via nf.settings).
local player_name = "Player"
local volume = 80
local profile_status = ""
local profile_ready = false

local function load_profile()
    local saved, ok, err = nf.settings.load()
    if ok and saved then
        if saved.player_name and saved.player_name ~= "" then player_name = saved.player_name end
        if saved.volume then
            local v = tonumber(saved.volume)
            if v and v >= 0 and v <= 100 then volume = v end
        end
        if saved.show_hint then show_hint = (saved.show_hint == "1" or saved.show_hint == "true") end
        profile_status = "Loaded / تم التحميل"
    elseif not ok then
        profile_status = "Load failed / فشل التحميل: " .. tostring(err)
    else
        profile_status = "No saved settings / لا إعدادات محفوظة"
    end
    profile_ready = true
end

function update(dt)
    nf.ui.cursor(true)
    if not profile_ready then
        load_profile()
    end
    local pr = panel_rect()
    local px, py, pw = pr.x + 0.025, pr.y + 0.03, pr.w - 0.05

    nf.ui.panel("panel", pr.x, pr.y, pr.w, pr.h)

    -- page tabs (work at every size: anchor keeps them inside the panel)
    if nf.ui.button("tab_main", "Main", px, py, 0.09, 0.055) then page = "main" end
    if nf.ui.button("tab_settings", "Settings", px + 0.105, py, 0.10, 0.055) then page = "settings" end
    if nf.ui.button("tab_demo", "Demo", px + 0.215, py, 0.09, 0.055) then page = "demo" end
    if nf.ui.button("tab_profile", "Profile", px + 0.315, py, 0.10, 0.055) then page = "profile" end

    if page == "main" then
        nf.ui.label("title", "SANAD UI LAB", px, py + 0.075, pw, 0.06, 28, "left")
        nf.ui.label("arabic", "مختبر واجهات الألعاب", px, py + 0.14, pw, 0.055, 22, "right")
        nf.ui.label("sub", "Native player / Lua controls", px, py + 0.20, pw, 0.045, 16, "left")
        nf.ui.bar("level", string.format("Charge: %d%%", math.floor(charge + 0.5)), charge / 100, px, py + 0.26, pw, 0.055)
        if nf.ui.button("charge", "+ Charge", px, py + 0.34, pw * 0.48, 0.06) then
            charge = math.min(100, charge + 20)
        end
        if nf.ui.button("drain", "- Drain", px + pw * 0.52, py + 0.34, pw * 0.48, 0.06) then
            charge = math.max(0, charge - 20)
        end
        if nf.ui.button("pause", paused and "Resume / متابعة" or "Pause / إيقاف", px, py + 0.42, pw, 0.06) then
            paused = not paused
        end
        load = nf.ui.slider("load", "Load: " .. math.floor(load + 0.5) .. "%", load, 0, 100, px, py + 0.50, pw, 0.05)
        if nf.ui.button("reset", "Reset / إعادة ضبط", px, py + 0.58, pw, 0.06) then
            charge, load, paused, show_hint, spin_speed, mode_index = 65, 40, false, true, 1.0, 1
        end
        nf.ui.label("state", paused and "Simulation paused" or "Simulation running", px, py + 0.67, pw, 0.04, 16, "left")
        nf.ui.label("wrap", "Long text wraps: هذا نص عربي طويل يلتف داخل اللوحة دون قص الحروف أو فصلها، mixed with numbers 123.",
            px, py + 0.72, pw, 0.10, 15, "left")
        nf.ui.tooltip("tip_hint", "Hover over buttons for help / مرّر فوق الأزرار للمساعدة", px, py + 0.72, pw, 0.10)
    elseif page == "settings" then
        nf.ui.label("settings_title", "Settings / الإعدادات", px, py + 0.075, pw, 0.05, 20, "left")
        show_hint = nf.ui.checkbox("show_hint", "Show hint bar / إظهار التلميح", show_hint, px, py + 0.14, pw, 0.055, true)
        spin_speed = nf.ui.slider("spin", "Spin speed: " .. string.format("%.1f", spin_speed), spin_speed, 0, 3, px, py + 0.22, pw, 0.05)
        if nf.ui.button("back1", "Back / رجوع", px, py + 0.72, pw, 0.06) then page = "main" end
    elseif page == "profile" then
        nf.ui.label("profile_title", "Profile & Settings / الملف والإعدادات", px, py + 0.075, pw, 0.05, 20, "left")
        -- Editable name (Arabic + English). Enter commits, Escape cancels.
        local new_name, committed, editing = nf.ui.text_input("player_name", player_name, px, py + 0.14, pw, 0.055, true, 48)
        if committed then
            player_name = new_name
            profile_status = "Name updated / تم تحديث الاسم"
        end
        nf.ui.label("name_hint", "Name (Enter commits, Esc cancels)", px, py + 0.20, pw, 0.04, 14, "left")
        -- Controlled number: volume 0..100, integer. Invalid drafts keep the old value.
        local new_vol, num_committed, num_valid, num_err = nf.ui.number_input("volume", "Volume", volume, 0, 100, px, py + 0.26, pw, 0.055, true, 5, true)
        if num_committed and num_valid then
            volume = new_vol
            profile_status = "Volume updated / تم تحديث مستوى الصوت"
        end
        if num_err and num_err ~= "" and not num_valid then
            profile_status = "Invalid: " .. tostring(num_err) .. " / غير صالح"
        end
        show_hint = nf.ui.checkbox("show_hint2", "Show hint bar / إظهار التلميح", show_hint, px, py + 0.34, pw, 0.055, true)
        nf.ui.label("vol_now", "Volume: " .. tostring(math.floor(volume + 0.5)) .. " / الحساسية: " .. tostring(math.floor(volume + 0.5)), px, py + 0.41, pw, 0.04, 16, "left")
        if nf.ui.button("save_profile", "Save / حفظ", px, py + 0.47, pw * 0.32, 0.06) then
            local ok, err = nf.settings.save({player_name = player_name, volume = tostring(volume), show_hint = show_hint and "1" or "0"})
            if ok then profile_status = "Saved / تم الحفظ"
            else profile_status = "Save failed / فشل الحفظ: " .. tostring(err) end
        end
        if nf.ui.button("load_profile", "Load / تحميل", px + pw * 0.34, py + 0.47, pw * 0.32, 0.06) then
            load_profile()
        end
        if nf.ui.button("restore_defaults", "Restore Defaults / استعادة الافتراضي", px, py + 0.56, pw, 0.06) then
            player_name, volume, show_hint = "Player", 80, true
            local ok, err = nf.settings.save({player_name = player_name, volume = tostring(volume), show_hint = "1"})
            if ok then profile_status = "Defaults restored / تمت استعادة الافتراضي"
            else profile_status = "Defaults failed / فشل: " .. tostring(err) end
        end
        nf.ui.label("profile_status", profile_status, px, py + 0.65, pw, 0.04, 15, "left")
        nf.ui.label("profile_path", "File: " .. tostring(nf.settings.path()), px, py + 0.69, pw, 0.04, 11, "left")
        if nf.ui.button("back3", "Back / رجوع", px, py + 0.74, pw, 0.06) then page = "main" end
    else
        nf.ui.label("demo_title", "Demo / قائمة تجريبية", px, py + 0.075, pw, 0.05, 20, "right")
        local sel, n = nf.ui.list("modes", modes, mode_index, px, py + 0.14, pw, 0.24, 4, true)
        mode_index = sel
        mode_count = n
        nf.ui.label("mode_now", "Mode: " .. modes[mode_index] .. " (" .. mode_index .. "/" .. n .. ")", px, py + 0.41, pw, 0.045, 16, "left")
        nf.ui.label("demo_note", "Try Tab + Enter, arrows, and wheel. Numbers 123 stay in order.", px, py + 0.47, pw, 0.08, 15, "left")
        if nf.ui.button("back2", "Back / رجوع", px, py + 0.72, pw, 0.06) then page = "main" end
    end

    if show_hint then
        nf.ui.label("hint", "Tab / Enter / arrows work too.", px, py + 0.82, pw, 0.04, 14, "left")
    end

    -- visual links (real, not cosmetic)
    if not paused then
        spin_t = spin_t + dt * spin_speed
        charge = math.max(0, charge - dt * load / 100 * 3 * spin_speed)
    end
    if mode_index == 2 then
        nf.set_entity_scale(self_id, self_gen, 1, 0.5 + charge / 50 + math.sin(spin_t * 4) * 0.25, 1)
    elseif mode_index == 3 then
        nf.set_entity_scale(self_id, self_gen, 1 + math.sin(spin_t * 2) * 0.4, 0.5 + charge / 50, 1)
    else
        nf.set_entity_scale(self_id, self_gen, 1, 0.5 + charge / 50, 1)
    end
end
