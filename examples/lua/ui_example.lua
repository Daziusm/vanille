local tab_id = ui.create_tab("ui example")
ui.clear_tab(tab_id)

ui.checkbox(tab_id, "enabled", "Enabled", true)
ui.colorpicker(tab_id, "accent", "Accent", { 0.15, 0.7, 1.0, 1.0 })

ui.checkbox(tab_id, "show_boxes", "Show Boxes", false)
ui.colorpicker(tab_id, "outline", "Outline", { 1.0, 1.0, 1.0, 1.0 })

ui.checkbox(tab_id, "hold_to_aim", "Hold To Aim", true)
ui.keybind(tab_id, "aim_key", "Aim Key", 0x02, 0)

ui.checkbox(tab_id, "toggle_menu", "Toggle Menu", true)
ui.keybind(tab_id, "menu_key", "Menu Key", 0x2D, 1)

ui.dropdown(tab_id, "mode", "Mode", { "Closest", "Fov", "Distance" }, 0)
ui.dropdown(tab_id, "hitpart", "Hitpart", { "Head", "Torso", "HumanoidRootPart" }, 0)

ui.slider(tab_id, "fov", "Fov", 25, 500, 120)
ui.slider(tab_id, "smoothness", "Smoothness", 1, 100, 20)
