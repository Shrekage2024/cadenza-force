#!/usr/bin/env bash
# Build Cadenza for the Force (armhf) with the mpc-vst-plugins toolchain:
#   vst/build/cadenza.so                      the plugin
#   vst/build/skin/Cadenza - VST - Cadenza/   the plugin folder's skin, and LICENSES.txt (Cadenza's and the toolchain's)
#   vst/build/pluginlist-entry.xml            the <PLUGIN> line for MPC.settings' pluginList-arm
# then checks the skin and binary against vst/budget.json (tests/budget.sh --offline).
# MPC_VST points at a checkout of sd88me/mpc-vst-plugins (default: next to this repo). Needs Docker.
# CFLAGS_EXTRA adds compiler flags to this one build (vst.json's cflags plus these, through a temporary copy of vst.json), e.g.
#   CFLAGS_EXTRA=-DTAP_LOG vst/build.sh    the image-button tap log (src/engine.c tap_log), a diagnostic build only
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../../mpc-vst-plugins}" && pwd)"
U="$(id -u):$(id -g)"
docker build -q -t mpc-vst-html-art "$MPC_VST/tools/html_art" >/dev/null
# 0.22: the toolchain's Titillium (a word drawn into the JAM bar meters' frames) read-only at /fonts
docker run --rm -u "$U" -e HOME=/tmp -e SKIN_FONTS=/fonts -v "$MPC_VST/tools/html_art/fonts":/fonts:ro -v "$PWD":/w -w /w \
  mpc-vst-html-art python3 make_skin.py
CFG="$PWD/vst.json"
if [ -n "${CFLAGS_EXTRA:-}" ]; then
  CFG="$PWD/.vst-extra.json"
  trap 'rm -f "$CFG"' EXIT
  python3 -c 'import json, sys; c = json.load(open("vst.json")); c["build"]["cflags"] += sys.argv[1].split(); json.dump(c, open(sys.argv[2], "w"), indent=1)' \
    "$CFLAGS_EXTRA" "$CFG"
fi
"$MPC_VST/tools/build_port.sh" "$CFG"
# The licences ship in the plugin folder, so the release zip and the Force carry them: the toolchain's MIT notice
# must go with cadenza.so, which links its wrapper.
{
  printf 'Cadenza (this plugin and its skin):\n\n'; cat ../LICENSE
  printf '\n\nmpc-vst-plugins (wrapper/vst2_wrap.c, compiled into cadenza.so):\n\n'; cat "$MPC_VST/LICENSE"
} > "$(echo build/skin/*/)LICENSES.txt"
MPC_VST="$MPC_VST" ../tests/budget.sh --offline
