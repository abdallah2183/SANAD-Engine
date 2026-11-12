-- Side-view movement and physics are handled by PlatformerController.
-- This script only presents the controls in the game HUD.
function update(dt)
    nf.ui.label("controls", "Move: A/D or arrows    Jump: Space", 0.025, 0.025, 0.60, 0.06, 20, "left")
end
