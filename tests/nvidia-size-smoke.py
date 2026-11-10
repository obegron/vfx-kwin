#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise captures larger than UHD, then resize recovery, in a private compositor."""
import json
import math
import os
from pathlib import Path
import subprocess
import time

from nested_support import artifacts, effects, script, screenshot, configure, toggle


def status():
    return json.loads(effects.debug("kwin_vfx","",signature="ss"))


def wait_for_output(large):
    deadline=time.monotonic()+60
    while time.monotonic()<deadline:
        state=status()
        assert not state["nvidia_error"],state
        for job in state["nvidia_jobs"]:
            width,height=job["input_size"]
            is_large=width>3840 and height>2160
            if is_large==large and job.get("published_revision",0)>0 and state["nvidia_draws"]>0:
                return job
        time.sleep(0.05)
    raise AssertionError(status())


assert status()["nvidia_available"],"Run through scripts/try-nvidia.sh"
build=Path(os.environ["KWIN_VFX_BUILD_DIR"])
client=subprocess.Popen([str(build/"kwin_vfx_smoke_client")],env=dict(os.environ,KWIN_VFX_TEST_PATTERN="1"))
try:
    time.sleep(1)
    scale=screenshot("initial-output").info["scale"]
    width,height=math.ceil(3904/scale),math.ceil(2224/scale)
    script(f"""
        const windows=workspace.windowList();
        const target=windows.find(w=>w.caption==='VFX test target');
        windows.find(w=>w.caption==='VFX test control').closeWindow();
        target.frameGeometry={{x:40,y:40,width:{width},height:{height}}};
        workspace.activeWindow=target;
    """)
    configure(strength=1,nvidia_mode=12,cas_only=False,cas_strength=0,film_grain=0)
    toggle()
    large=wait_for_output(True)
    script("workspace.activeWindow.frameGeometry={x:40,y:40,width:360,height:240};")
    small=wait_for_output(False)
    assert status()["selected"]==1
    toggle()
    assert status()["selected"]==0
    report={"passed":True,"large_capture":large["input_size"],"small_capture":small["input_size"],
            "resize_recovered":True,"mode":12,"scale":scale}
    (artifacts/"nvidia-size-result.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps(report),flush=True)
finally:
    client.terminate();client.wait(timeout=10)
