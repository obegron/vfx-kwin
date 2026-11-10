#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""TrueHDR look tunables apply live: no worker restart, visible output change."""
import json
import os
from pathlib import Path
import subprocess
import time

from PIL import ImageChops, ImageStat
from nested_support import artifacts, effects, script, screenshot, toggle, configure


def debug():
    return json.loads(effects.debug("kwin_vfx", "", signature="ss"))


def wait(predicate, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = debug()
        assert not state["nvidia_error"] and not state["processing_error"], state
        if predicate(state):
            return state
        time.sleep(0.05)
    raise AssertionError(debug())


def crop(image, box):
    return image.crop(tuple(round(v * image.info["scale"]) for v in box)).convert("RGB")


def spread(image, box):
    """Mean per-pixel channel spread: a saturation proxy."""
    pixels = list(crop(image, box).getdata())
    return sum(max(p) - min(p) for p in pixels) / len(pixels)


def look(**values):
    configure(strength=1, denoise=0, sharpen=0, film_grain=0, true_hdr_sdr=True, extra=values)


def published(state):
    jobs = state["nvidia_jobs"]
    return jobs[0]["published_revision"] if jobs else 0


def settle(before):
    """Wait for a TrueHDR result newer than `before` to be drawn."""
    return wait(lambda d: d["nvidia_jobs"] and d["nvidia_jobs"][0]["true_hdr"]
                and published(d) > published(before) and d["nvidia_draws"] > before["nvidia_draws"])


assert debug()["nvidia_available"], "Run through scripts/try-nvidia.sh"
client = subprocess.Popen([str(Path(os.environ["KWIN_VFX_BUILD_DIR"]) / "kwin_vfx_smoke_client")],
                          env=dict(os.environ, KWIN_VFX_TEST_PATTERN="1", KWIN_VFX_TEST_COLOR="1"))
try:
    time.sleep(1)
    script("""
        const target = workspace.windowList().find(w => w.caption === 'VFX test target');
        target.frameGeometry = {x:60,y:80,width:360,height:240};
        workspace.activeWindow = target;
    """)
    look(HdrContrast=100, HdrSaturation=100, HdrMiddleGray=50, HdrDebanding=True)
    toggle()
    box = (85, 105, 395, 300)
    start = wait(lambda d: d["nvidia_draws"] > 0 and d["nvidia_jobs"] and d["nvidia_jobs"][0]["true_hdr"]
                 and d["nvidia_jobs"][0]["state"] == 4)
    starts = start["ai_slot_starts"]
    default = screenshot("truehdr-look-default")

    look(HdrContrast=100, HdrSaturation=0, HdrMiddleGray=50, HdrDebanding=True)
    state = settle(start)
    desaturated = screenshot("truehdr-look-desaturated")
    assert spread(desaturated, box) < spread(default, box) * 0.8, (spread(default, box), spread(desaturated, box))

    look(HdrContrast=100, HdrSaturation=100, HdrMiddleGray=90, HdrDebanding=True)
    state = settle(state)
    bright = screenshot("truehdr-look-bright")
    brightness = lambda image: ImageStat.Stat(crop(image, box).convert("L")).mean[0]
    assert brightness(bright) > brightness(default) + 5, ("Higher middle gray did not brighten", brightness(default), brightness(bright))

    look(HdrContrast=100, HdrSaturation=100, HdrMiddleGray=50, HdrDebanding=False)
    state = settle(state)
    assert state["ai_slot_starts"] == starts, ("look change restarted the worker", starts, state)
    assert not state["hdr_debanding"]
    result = {"passed": True, "live_without_restart": True,
              "spread_default": spread(default, box), "spread_desaturated": spread(desaturated, box),
              "brightness_default": brightness(default), "brightness_middle_gray_90": brightness(bright)}
    (artifacts / "truehdr-look-result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
finally:
    client.terminate()
    client.wait(timeout=10)
