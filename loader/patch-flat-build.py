import os
import shutil

ROOT = os.path.join(os.path.dirname(__file__), "..", "Vanille", "source")

def write_file(path, data):
    with open(path, "r", encoding="utf-8") as f:
        if f.read() == data:
            print(f"unchanged {path}")
            return True
    fd = os.open(path, os.O_WRONLY | os.O_TRUNC)
    try:
        os.write(fd, data.encode("utf-8"))
    finally:
        os.close(fd)
    print(f"updated {path}")
    return True

def patch_replace(path, replacements):
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()
    original = content
    for old, new in replacements:
        content = content.replace(old, new)
    if content == original:
        print(f"skip {path}")
        return
    write_file(path, content)

colors_src = os.path.join(ROOT, "gui", "colors", "colors_new.h")
colors_dst = os.path.join(ROOT, "gui", "colors", "colors.h")
with open(colors_src, "r", encoding="utf-8") as f:
    colors_data = f.read()
write_file(colors_dst, colors_data)

shim = os.path.join(os.path.dirname(__file__), "..", "Vanille", "source_include_shim", "gui", "colors", "colors.h")
if os.path.isdir(os.path.dirname(shim)):
    write_file(shim, colors_data)

patch_replace(
    os.path.join(ROOT, "gui", "widgets", "keybind", "keybind.cpp"),
    [("../../colors/colors.h", "../../colors/colors_new.h")],
)
patch_replace(
    os.path.join(ROOT, "gui", "assistant.cpp"),
    [('colors/colors.h', 'colors/colors_new.h')],
)
patch_replace(
    os.path.join(ROOT, "features", "tests.cpp"),
    [("gui/colors/colors.h", "gui/colors/colors_new.h")],
)

overlay_path = os.path.join(ROOT, "gui", "overlay.cpp")
with open(overlay_path, "r", encoding="utf-8") as f:
    overlay = f.read()

overlay_old = """        c_colors::top_window_background = adjust(t.window, -delta * 0.35f);
        c_colors::bottom_window_background = adjust(t.window, delta * 0.35f);

        c_colors::top_child_background = adjust(t.child, delta * 0.35f);
        c_colors::bottom_child_background = c_colors::derive_bottom_surface(c_colors::top_child_background, 0.012f);

        if (preset_index == 5)
            c_colors::bottom_child_background = c_colors::derive_bottom_surface(c_colors::top_child_background, 0.010f);

        if (preset_index == 6)
            c_colors::bottom_child_background = c_colors::derive_bottom_surface(c_colors::top_child_background, 0.012f);"""

overlay_new = """        c_colors::top_window_background = adjust(t.window, -delta * 0.35f);
        c_colors::bottom_window_background = c_colors::top_window_background;

        c_colors::top_child_background = adjust(t.child, delta * 0.35f);
        c_colors::bottom_child_background = c_colors::top_child_background;"""

if overlay_old in overlay:
    overlay = overlay.replace(overlay_old, overlay_new)

overlay = overlay.replace(
    "style.Colors[ImGuiCol_WindowBg] = c_colors::bottom_window_background;",
    "style.Colors[ImGuiCol_WindowBg] = c_colors::top_window_background;",
)
overlay = overlay.replace(
    "style.Colors[ImGuiCol_ChildBg] = c_colors::bottom_child_background;",
    "style.Colors[ImGuiCol_ChildBg] = c_colors::top_child_background;",
)
overlay = overlay.replace(
    "style.Colors[ImGuiCol_FrameBg] = c_colors::bottom_child_background;",
    "style.Colors[ImGuiCol_FrameBg] = c_colors::top_child_background;",
)
overlay = overlay.replace(
    "style.Colors[ImGuiCol_Button] = c_colors::bottom_child_background;",
    "style.Colors[ImGuiCol_Button] = c_colors::top_child_background;",
)
write_file(overlay_path, overlay)
