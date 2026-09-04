# -*- coding: utf-8 -*-
"""Post-config extra_scripts for RLCD (no pre: prefix).

Loads via env.SConscript() AFTER the build script (see platformio builder
main.py:167/180), which means libdeps are already downloaded and
  $PROJECT_LIBDEPS_DIR/<env>/lvgl/src/core/lv_refr.c
exists on disk. We apply the one-line device customization directly at import
time (top-level code), so it runs before LVGL objects are compiled.

Local Waveshare lvgl8 carried one device customization over stock 8.4.0:
  src/core/lv_refr.c  (rotated refresh loop)  -- force even flush chunk height:
      while(row < area_h) {
          lv_coord_t height = LV_MIN(max_row, area_h - row);
          height &= ~0x1UL;          <-- local-only line
The 400x300 reflective panel rotation path requires even flush heights (ST7305
even-row driver expectation). Without it, stock 8.4.0 uses odd chunk heights.

IMPORTANT: use in platformio.ini WITHOUT a pre:/post: prefix:
  extra_scripts = scripts/patch_lvgl.py
A "pre:" prefix loads the script BEFORE $BUILD_SCRIPT, i.e. before libdeps are
downloaded, so lv_refr.c is not yet present on disk.
"""
import os
from platformio import util  # noqa

Import("env")  # SConscript exports="env"


def _find_lv_refr():
    """Locate lv_refr.c under the libdeps install dir."""
    libdeps = env.subst("$PROJECT_LIBDEPS_DIR")
    search = [libdeps]
    libdir = env.subst("$PROJECT_LIB_DIR")
    if libdir and os.path.isdir(libdir):
        search.append(libdir)
    for base in search:
        if not base or not os.path.isdir(base):
            continue
        for dirpath, _dirnames, filenames in os.walk(base):
            if "lv_refr.c" in filenames and dirpath.replace(os.sep, "/").endswith("lvgl/src/core"):
                return os.path.join(dirpath, "lv_refr.c")
    return None


def _patch_lv_refr(path):
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()
    marker = "lv_coord_t height = LV_MIN(max_row, area_h - row);"
    add = "            height &= ~0x1UL;\n"
    if marker not in src:
        raise RuntimeError(f"[lvgl-patch] anchor not found: {path}")
    if "height &= ~0x1UL;" in src:
        print("[lvgl-patch] lv_refr.c already patched")
        return
    src = src.replace(marker, marker + "\n" + add, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    print("[lvgl-patch] lv_refr.c even-flush-height patch applied")


try:
    path = _find_lv_refr()
    print(f"[lvgl-patch] lv_refr.c = {path!r}")
    if not path:
        print("[lvgl-patch] lv_refr.c not found (LVGL not installed?)")
    else:
        _patch_lv_refr(path)
except Exception as e:
    # Non-fatal warning: build still proceeds; patch matters only for this board
    print(f"[lvgl-patch] WARN: {e}")
