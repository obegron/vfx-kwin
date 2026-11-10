#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The filter pass is cached: repaints of static content only redraw grain."""
import json
import os
from pathlib import Path
import subprocess
import time

from nested_support import effects, script, screenshot, toggle, configure


def debug():
    return json.loads(effects.debug("kwin_vfx", "", signature="ss"))


def wait(predicate):
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        state = debug()
        assert not state["nvidia_error"] and not state["processing_error"], state
        if predicate(state):
            return state
        time.sleep(0.05)
    raise AssertionError(debug())


client = subprocess.Popen([str(Path(os.environ["KWIN_VFX_BUILD_DIR"]) / "kwin_vfx_smoke_client")],
                          env=dict(os.environ, KWIN_VFX_TEST_PATTERN="1"))
try:
    time.sleep(1)
    script("""
        const target = workspace.windowList().find(w => w.caption === 'VFX test target');
        target.frameGeometry = {x:60,y:80,width:360,height:240};
        workspace.activeWindow = target;
    """)
    configure(strength=1, denoise=0.5, sharpen=0.5, film_grain=1)
    toggle()
    if debug()["backend"] == "nvidia-experimental":
        wait(lambda d: d["nvidia_draws"] > 0 and d["nvidia_jobs"] and all(j["state"] == 4 for j in d["nvidia_jobs"]))
    start = wait(lambda d: d["process_draws"] > 0 and d["grain_animating"])
    time.sleep(1.0)  # ~24 grain repaints of an unchanged window.
    idle = debug()
    frames = (idle["grain_frame"] - start["grain_frame"]) % 4096
    assert frames >= 10, ("grain did not drive repaints", start, idle)
    assert idle["process_draws"] - start["process_draws"] <= 1, ("filter reran on static content", start, idle)
    # Changing a filter parameter must invalidate the cache without a recapture.
    configure(strength=1, denoise=0.5, sharpen=1, film_grain=1)
    wait(lambda d: d["process_draws"] > idle["process_draws"])
    screenshot("process-cache-updated")
    print(json.dumps({"passed": True, "repaints": frames,
                      "filter_passes": idle["process_draws"] - start["process_draws"]}))
finally:
    client.terminate()
    client.wait()
