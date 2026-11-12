-- Main menu. Settings load once; a scene change is queued until this callback returns.
local page, ready, status = "main", false, ""
local player_name, time_limit = "Player", 20
local function load_profile()
    local saved, ok, err = nf.settings.load()
    if ok and saved then
        if saved.player_name and saved.player_name ~= "" then player_name = saved.player_name end
        local value = tonumber(saved.time_limit)
        if value and value >= 5 and value <= 60 then time_limit = math.floor(value) end
    elseif not ok then status = "Load failed: " .. tostring(err) end
    ready = true
end
local function new_game()
    nf.session.clear()
    nf.session.set("player_name", player_name)
    nf.session.set("time_limit", time_limit)
    local ok, err = nf.scene.load("content://Scenes/DemoLevel.nfscene")
    if not ok then status = tostring(err) end
end
function update(dt)
    nf.ui.cursor(true)
    if not ready then load_profile() end
    local x,y,w,h = nf.ui.anchor_rect("topleft", .46,.90,.035,320,240)
    nf.ui.panel("panel",x,y,w,h)
    local px,py,pw = x+.025,y+.035,w-.05
    nf.ui.label("title","UI STARTER",px,py,pw,.07,28)
    nf.ui.label("arabic","بداية لعبتك",px,py+.075,pw,.06,24,"right")
    if page == "main" then
        nf.ui.label("welcome","Hello " .. player_name,px,py+.16,pw,.055,20)
        nf.ui.label("goal","Activate 3 beacons before time runs out.",px,py+.23,pw,.10,18)
        if nf.ui.button("play","New Game / لعبة جديدة",px,py+.36,pw,.075) then new_game() end
        if nf.ui.button("settings","Settings / الإعدادات",px,py+.46,pw,.075) then page="settings" end
        if nf.ui.button("quit","Exit / خروج",px,py+.56,pw,.075) then nf.scene.quit() end
    else
        nf.ui.label("name_label","Player name / اسم اللاعب",px,py+.16,pw,.05,18)
        local name,committed = nf.ui.text_input("name",player_name,px,py+.22,pw,.065,true,32)
        if committed then player_name=name end
        local n,commit,valid = nf.ui.number_input("limit","Time limit (5-60 s)",time_limit,5,60,px,py+.32,pw,.07,true,5,true)
        if commit and valid then time_limit=n end
        if nf.ui.button("save","Save / حفظ",px,py+.43,pw*.48,.07) then
            local ok,err=nf.settings.save({player_name=player_name,time_limit=time_limit})
            status=ok and "Saved / تم الحفظ" or tostring(err)
        end
        if nf.ui.button("defaults","Defaults",px+pw*.52,py+.43,pw*.48,.07) then player_name="Player";time_limit=20 end
        if nf.ui.button("back","Back / رجوع",px,py+.54,pw,.07) then page="main" end
    end
    local state,target,err=nf.scene.status()
    if state=="failed" then status="Could not load scene: " .. err end
    nf.ui.label("status",status,px,py+.67,pw,.08,15)
    nf.ui.label("hint","Tab / Enter or mouse",px,py+.78,pw,.04,14)
end
