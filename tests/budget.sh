#!/usr/bin/env bash
# Resource budget gate, with the limits in vst/budget.json. Run after vst/build.sh; exits 1 if anything is over.
#   - skin images, decoded the way MPC holds them (width x height x 4 bytes)
#   - plugin binary size
#   - the last device bench (vst/bench.txt): p99 and worst block, worker threads, page open time. These limits are
#     stricter than bench.sh's own PASS (15% / 50%): a sequencer shares the Force with the instruments it drives.
#   - what the plugin page costs MPC's UI thread while playing (vst/ui_cost.json, measured with tools/soak.sh)
#     --offline skips the device numbers (vst/build.sh runs it that way, since they may predate the build).
# Screen updates while playing are checked by tests/test_plugin.c; the soak on the device is tools/soak.sh.
# MPC_VST points at a checkout of sd88me/mpc-vst-plugins (default: next to this repo).
set -euo pipefail
cd "$(dirname "$0")/.."
MPC_VST="$(cd "${MPC_VST:-../mpc-vst-plugins}" && pwd)"
python3 - "$MPC_VST/tools" "${1:-}" <<'EOF'
import glob, json, os, sys

sys.path.insert(0, sys.argv[1])
from skin_assets import image_size, is_image

limit = json.load(open("vst/budget.json"))
rows = []
def check(name, value, key, unit, fmt="%.1f"):
    rows.append((name, fmt % value + unit, fmt % limit[key] + unit, value <= limit[key]))

files = [f for f in glob.glob("vst/build/skin/*/Plugin Skins/*") if is_image(f)]
if not files:
    sys.exit("no skin in vst/build: run vst/build.sh first")
skin = sum(w * h * 4 for w, h in map(image_size, files))
check("skin images, decoded", skin / 1e6, "skin_mb", " MB")
check("plugin binary", os.path.getsize("vst/build/cadenza.so") / 1024, "binary_kb", " KB", "%.0f")

if sys.argv[2] != "--offline":
    bench = json.loads(open("vst/bench.txt").read().strip().splitlines()[-1])
    check("audio CPU, p99 block", bench["p99_pct"], "p99_pct", "%")
    check("audio CPU, worst block", bench["max_pct"], "worst_pct", "%")
    check("worker threads", bench["threads_pct"], "threads_pct", "%")
    check("page open", bench["open_ms"], "open_ms", " ms")
    ui = json.load(open("vst/ui_cost.json"))
    check("UI thread, page open", ui["main_thread_page_open_pct"] - ui["main_thread_page_closed_pct"], "ui_thread_pct", "%")

for name, value, lim, ok in rows:
    print("%-4s %-24s %10s  (budget %s)" % ("ok" if ok else "OVER", name, value, lim))
over = sum(not r[3] for r in rows)
print("\nOVER BUDGET: %d" % over if over else "\nBUDGET OK")
sys.exit(1 if over else 0)
EOF
