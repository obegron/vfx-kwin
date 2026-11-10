#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Filter a local Brave page in a private compositor, including PQ-encoded SDR."""
import json
import os
from pathlib import Path
import signal
import subprocess
import time

import dbus
from nested_support import artifacts, bus, configure, effects


def state():
    return json.loads(effects.debug("kwin_vfx", "windows", signature="ss"))


configure(strength=1, cas_only=False, nvidia_mode=12, cas_strength=0.5)
page = artifacts / "brave-fixture.html"
page.write_text("""<!doctype html><title>VFX Brave fixture</title>
<style>body{background:#888}canvas{width:95vw;height:85vh}</style><canvas></canvas>
<script>
const canvas=document.querySelector('canvas'), ctx=canvas.getContext('2d');
const pixels=ctx.createImageData(canvas.width,canvas.height);
let seed=42;
for(let i=0;i<pixels.data.length;i+=4){
    seed=(Math.imul(seed,1664525)+1013904223)>>>0;
    pixels.data[i]=pixels.data[i+1]=pixels.data[i+2]=110+(seed%36);
    pixels.data[i+3]=255;
}
ctx.putImageData(pixels,0,0);
</script>""")
with (artifacts / "brave.log").open("w") as log:
    browser = subprocess.Popen([
        "brave", "--user-data-dir=" + str(artifacts / "brave-profile"),
        "--no-first-run", "--disable-sync", "--disable-background-networking",
        "--ozone-platform=wayland", "--new-window", page.as_uri(),
    ], stdout=log, stderr=log, start_new_session=True)
    try:
        control = dbus.Interface(bus.get_object("org.kde.KWin", "/WindowEnhancement"),
                                 "io.github.obegron.vfxkwin.WindowEnhancement")
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            windows = json.loads(control.windowList())
            matches = [w for w in windows if "brave" in w["application"].lower()]
            if matches:
                break
            time.sleep(0.1)
        assert matches, windows
        window_id = matches[0]["id"]
        time.sleep(1)
        before = state()
        source = next(w for w in before["windows"] if w["id"] == window_id)
        pq_sdr = any(item["transfer"] == 2 and item["max_hdr_nits"] <= item["reference_nits"] * 1.01
                     for item in source["items"])
        if os.environ.get("KWIN_VFX_REQUIRE_PQ_SDR"):
            assert before["hdr_available"] and pq_sdr, before
        assert control.setWindowEnabled(window_id, True, signature="sb")
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            current = state()
            assert current["selected"] == 1, current
            assert not current["processing_error"] and not current["nvidia_error"], current
            if current["nvidia_draws"] > 0:
                break
            time.sleep(0.05)
        assert current["nvidia_draws"] > 0, current
        assert current["nvidia_jobs"] and not current["nvidia_jobs"][0]["true_hdr"], current
        # Selection must survive subsequent repaints, rather than being removed
        # after setWindowEnabled has already returned success.
        time.sleep(1)
        assert state()["selected"] == 1, state()
        assert control.setWindowEnabled(window_id, False, signature="sb")
        assert state()["selected"] == 0
        result = {"passed": True, "pq_encoded_sdr": pq_sdr,
                  "hdr_display": before["hdr_available"], "source": source,
                  "nvidia_draws": current["nvidia_draws"], "selection_retained": True}
        (artifacts / "brave-result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result), flush=True)
    finally:
        os.killpg(browser.pid, signal.SIGTERM)
        browser.wait(timeout=10)
