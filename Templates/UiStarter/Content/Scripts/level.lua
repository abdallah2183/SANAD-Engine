-- Small interaction trial; scene/session flow is generic engine infrastructure.
local elapsed, score, done = 0, 0, false
local activated = {}
local function finish(won)
    if done then return end
    nf.session.set("score",score)
    nf.session.set("elapsed",elapsed)
    nf.session.set("won",won)
    local ok = nf.scene.load("content://Scenes/Results.nfscene")
    done=ok
end
function update(dt)
    nf.ui.cursor(true)
    local limit=nf.session.get("time_limit",20)
    if not done then elapsed=elapsed+dt end
    local x,y,w,h=nf.ui.anchor_rect("topleft",.43,.90,.035,300,240)
    nf.ui.panel("panel",x,y,w,h)
    local px,py,pw=x+.025,y+.035,w-.05
    nf.ui.label("title","BEACON TRIAL",px,py,pw,.07,26)
    nf.ui.label("ar","فعّل الإشارات الثلاث",px,py+.08,pw,.06,23,"right")
    nf.ui.bar("time",string.format("Time left: %.1f s",math.max(0,limit-elapsed)),math.max(0,1-elapsed/limit),px,py+.17,pw,.065)
    nf.ui.label("score","Beacons: " .. score .. " / 3",px,py+.26,pw,.06,20)
    for i=1,3 do
        local text=activated[i] and ("Beacon "..i.." - ACTIVE") or ("Activate beacon "..i)
        if nf.ui.button("beacon"..i,text,px,py+.34+(i-1)*.095,pw,.075,not activated[i] and not done) then
            activated[i]=true;score=score+1
            local id,gen=nf.find_entity("Beacon"..i)
            if id then nf.set_entity_scale(id,gen,.25,1.8,.25) end
        end
    end
    if nf.ui.button("restart","Restart / إعادة",px,py+.66,pw*.48,.065) then nf.scene.restart() end
    if nf.ui.button("menu","Menu / القائمة",px+pw*.52,py+.66,pw*.48,.065) then nf.scene.load("content://Scenes/Main.nfscene") end
    local state,target,err=nf.scene.status()
    if state=="failed" then nf.ui.label("error",err,px,py+.75,pw,.07,14) end
    if score==3 then finish(true) elseif elapsed>=limit then finish(false) end
end
