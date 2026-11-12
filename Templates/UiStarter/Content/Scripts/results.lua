function update(dt)
    nf.ui.cursor(true)
    local x,y,w,h=nf.ui.anchor_rect("center",.52,.76,.03,340,260)
    nf.ui.panel("panel",x,y,w,h)
    local px,py,pw=x+.03,y+.04,w-.06
    local won=nf.session.get("won",false)
    nf.ui.label("title",won and "TRIAL COMPLETE" or "TIME IS UP",px,py,pw,.08,28,"center")
    nf.ui.label("ar",won and "اكتملت التجربة" or "انتهى الوقت",px,py+.09,pw,.07,25,"center")
    nf.ui.label("player",nf.session.get("player_name","Player"),px,py+.19,pw,.06,22,"center")
    nf.ui.label("score","Beacons: "..nf.session.get("score",0).." / 3",px,py+.27,pw,.06,22,"center")
    nf.ui.label("time",string.format("Elapsed: %.1f s",nf.session.get("elapsed",0)),px,py+.35,pw,.06,19,"center")
    if nf.ui.button("retry","Try again / حاول مجددًا",px,py+.45,pw,.075) then nf.scene.load("content://Scenes/DemoLevel.nfscene") end
    if nf.ui.button("menu","Main menu / القائمة الرئيسية",px,py+.55,pw,.075) then nf.scene.load("content://Scenes/Main.nfscene") end
    local state,target,err=nf.scene.status()
    if state=="failed" then nf.ui.label("error",err,px,py+.64,pw,.06,14) end
end
