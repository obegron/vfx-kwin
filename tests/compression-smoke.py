#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compression cleanup selection, capture reuse, bypass and pipeline transitions."""
import json
import os
from pathlib import Path
import subprocess
import time

from PIL import ImageChops, ImageStat
from nested_support import artifacts, effects, script, screenshot, toggle, configure


def state():
    return json.loads(effects.debug("kwin_vfx", "", signature="ss"))


def difference(a, b, box):
    rect = tuple(round(v * a.info["scale"]) for v in box)
    return ImageChops.difference(a.crop(rect), b.crop(rect))


def maximum(a, b, box):
    return max(pair[1] for pair in difference(a, b, box).getextrema())


client = subprocess.Popen([str(Path(os.environ["KWIN_VFX_BUILD_DIR"]) / "kwin_vfx_smoke_client")],
                          env=dict(os.environ, KWIN_VFX_TEST_PATTERN="1", KWIN_VFX_TEST_COMPRESSION="1"))
try:
    time.sleep(1)
    script("""
        const ws = workspace.windowList();
        const target = ws.find(w => w.caption === 'VFX test target');
        ws.find(w => w.caption === 'VFX test control').frameGeometry = {x:520,y:80,width:360,height:240};
        target.frameGeometry = {x:60,y:80,width:360,height:240};
        workspace.activeWindow = target;
    """)
    target, control, solid = (85,105,210,170), (545,105,670,170), (335,115,395,170)
    configure(strength=1, cas_only=True, cas_strength=0, compression_cleanup=0)
    baseline = screenshot("cleanup-original")
    toggle()
    configure(strength=1, cas_only=True, cas_strength=0, compression_cleanup=1)
    cleaned = screenshot("cleanup-max")
    current = state()
    assert current["selected"] == 1 and current["processing"] and not current["processing_error"], current
    assert current["cleanup_draws"] > 0 and not current["nvidia_jobs"], current
    assert maximum(baseline, cleaned, target) > 0, "Cleanup did not change dark block boundaries"
    assert maximum(baseline, cleaned, control) == 0, "Unselected window changed"
    assert maximum(baseline, cleaned, solid) <= 1, "Solid fill changed or capture orientation flipped"
    configure(strength=.5, cas_only=True, cas_strength=0, compression_cleanup=1)
    half = screenshot("cleanup-half-strength")
    assert 0 < sum(ImageStat.Stat(difference(baseline,half,target)).mean) < sum(ImageStat.Stat(difference(baseline,cleaned,target)).mean)
    configure(strength=0, cas_only=True, cas_strength=0, compression_cleanup=1)
    assert maximum(baseline, screenshot("cleanup-paused"), target) == 0
    assert state()["selected"] == 1 and not state()["processing"]
    configure(strength=1, cas_only=True, cas_strength=0, compression_cleanup=0)
    assert maximum(baseline, screenshot("cleanup-off"), target) == 0
    configure(strength=1, cas_only=True, cas_strength=0, compression_cleanup=1, film_grain=1)
    screenshot("cleanup-grain-start")
    draws = state()["cleanup_draws"]
    time.sleep(.3)
    assert state()["cleanup_draws"] == draws, "Static capture was cleaned again on grain-only repaints"
    configure(strength=1, cas_only=True, cas_strength=1, compression_cleanup=1)
    script("workspace.activeWindow.frameGeometry = {x:60,y:80,width:420,height:280};")
    screenshot("cleanup-resize-cas")
    assert state()["selected"] == 1 and not state()["processing_error"], state()
    ai = False
    hdr = False
    if state()["nvidia_available"]:
        configure(strength=1, cas_only=False, nvidia_mode=8, cas_strength=.25, compression_cleanup=1)
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            current = state()
            assert current["selected"] == 1 and not current["nvidia_error"] and not current["processing_error"], current
            if current["nvidia_draws"] > 0:
                break
            time.sleep(.05)
        assert current["nvidia_draws"] > 0, current
        ai = True
        old_draws = current["nvidia_draws"]
        configure(strength=1, cas_only=False, nvidia_mode=8, cas_strength=.25,
                  compression_cleanup=1, true_hdr_sdr=True)
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            current = state()
            assert current["selected"] == 1 and not current["nvidia_error"] and not current["processing_error"], current
            if current["nvidia_draws"] > old_draws and current["nvidia_jobs"] and current["nvidia_jobs"][0]["true_hdr"]:
                break
            time.sleep(.05)
        assert current["nvidia_draws"] > old_draws and current["nvidia_jobs"][0]["true_hdr"], current
        hdr = True
    toggle()
    assert state()["selected"] == 0
    result = {"passed": True, "dark_blocks_smoothed": True, "unselected_unchanged": True,
              "orientation_and_solid_fill": True, "strength_scales_cleanup": True,
              "off_and_pause_restore_original": True, "static_capture_reused": True,
              "resize_and_cas": True, "nvidia_transition": ai, "truehdr_transition": hdr}
    (artifacts / "compression-result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
finally:
    client.terminate()
    client.wait(timeout=10)
