import os

ROOT = os.path.join(os.path.dirname(__file__), "..", "Vanille", "source")

def copy_with_replace(src_rel, dst_rel, replacements):
    src = os.path.join(ROOT, src_rel)
    dst = os.path.join(ROOT, dst_rel)
    with open(src, "r", encoding="utf-8") as f:
        content = f.read()
    for old, new in replacements:
        content = content.replace(old, new)
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print(f"wrote {dst_rel}")

copy_with_replace(
    "entry_point.cpp",
    "entry_point.build.cpp",
    [('globals/globals.h', 'globals/globals_fixed.h')],
)

copy_with_replace(
    "features/triggerbot.cpp",
    "features/triggerbot.build.cpp",
    [('globals/globals.h', 'globals/globals_fixed.h')],
)

tests_build = os.path.join(ROOT, "features", "tests.build.cpp")
with open(tests_build, "r", encoding="utf-8") as f:
    content = f.read()
content = content.replace('globals/globals.h', 'globals/globals_fixed.h')
with open(tests_build, "w", encoding="utf-8", newline="\n") as f:
    f.write(content)
print("patched features/tests.build.cpp")
