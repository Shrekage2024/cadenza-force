#!/usr/bin/env bash
# Offline tests, no device needed, all under ASan/UBSan:
#   1. the sequencer core (timing, notes, mute, state), theory (borrowed chords, colours, voicing), the generator
#      (nesting, rules, golden checksum), the melody recipe generator and the MELODY knob audit
#   2. the plugin through the toolchain's real VST2 wrapper (tap, labels, save/restore, transport)
#   3. the toolchain's generic host test (tools/test_port.sh)
#   4. the core-API parity trace (tests/core_parity.c: the seq_* API alone, 44.1 and 48 kHz) against tests/core_parity.sha256
# MPC_VST points at a checkout of sd88me/mpc-vst-plugins (default: next to this repo).
set -euo pipefail
cd "$(dirname "$0")/.."
MPC_VST="$(cd "${MPC_VST:-../mpc-vst-plugins}" && pwd)"
mkdir -p build
python3 "$MPC_VST/tools/gen_vst.py" vst/vst.json --params-h >/dev/null
SCREEN_BUDGET=$(python3 -c 'import json; print(json.load(open("vst/budget.json"))["screen_updates_per_s"])')
docker run --rm -e SCREEN_BUDGET="$SCREEN_BUDGET" -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w gcc:12 bash -euc '
  SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
  gcc -std=gnu11 -Wall -Wextra -Werror $SAN -o build/test_core tests/test_core.c src/seq_core.c src/theory.c src/gen.c src/melody.c src/voicing.c src/rhythm.c src/jamlib.c src/bassline.c src/melody2.c -lm
  ./build/test_core
  gcc -std=gnu11 -Wall -Wextra -Werror $SAN -o build/test_chord_tone tests/test_chord_tone.c src/theory.c src/gen.c src/melody.c src/voicing.c src/rhythm.c src/jamlib.c src/bassline.c src/melody2.c -lm
  ./build/test_chord_tone
  gcc -std=gnu11 -Wall -Wextra -Werror $SAN -o build/test_theory tests/test_theory.c src/theory.c
  ./build/test_theory
  gcc -std=gnu11 -Wall -Wextra -Werror $SAN -o build/test_gen tests/test_gen.c src/gen.c src/theory.c -lm
  ./build/test_gen
  gcc -std=gnu11 -Wall -Wextra -Werror $SAN -o build/test_melody tests/test_melody.c src/melody.c src/gen.c src/theory.c -lm
  ./build/test_melody
  gcc -std=gnu11 -Wall -Wextra -Werror $SAN -o build/test_rhythm tests/test_rhythm.c src/rhythm.c src/jamlib.c src/bassline.c src/melody2.c src/seq_core.c src/theory.c src/gen.c src/melody.c src/voicing.c -lm
  ./build/test_rhythm
  gcc -std=gnu11 -Wall -Wextra -Werror $SAN -o build/test_bassline tests/test_bassline.c src/bassline.c src/melody2.c src/rhythm.c src/jamlib.c src/seq_core.c src/theory.c src/gen.c src/melody.c src/voicing.c -lm
  ./build/test_bassline
  gcc -std=gnu11 -Wall -Wextra -Werror $SAN -o build/test_melody2 tests/test_melody2.c src/melody2.c src/bassline.c src/rhythm.c src/jamlib.c src/theory.c -lm
  ./build/test_melody2
  gcc -std=gnu11 -Wall -Wextra -Werror $SAN -o build/test_jamlib tests/test_jamlib.c src/jamlib.c src/bassline.c src/rhythm.c src/theory.c src/seq_core.c src/gen.c src/melody.c src/voicing.c src/melody2.c -lm
  ./build/test_jamlib
  gcc -std=gnu11 -Wall -Wextra -Werror $SAN -o build/test_knob_audit tests/test_knob_audit.c src/seq_core.c src/theory.c src/gen.c src/melody.c src/voicing.c src/rhythm.c src/jamlib.c src/bassline.c src/melody2.c -lm
  ./build/test_knob_audit
  gcc -std=gnu11 -Wall -Wextra -Werror $SAN -o build/test_presets tests/test_presets.c src/presets.c src/seq_core.c src/theory.c src/gen.c src/melody.c src/voicing.c src/rhythm.c src/jamlib.c src/bassline.c src/melody2.c -lm
  ./build/test_presets
  gcc -std=gnu11 $SAN -DSCREEN_BUDGET=$SCREEN_BUDGET -Ivst/build -I/mv/wrapper -Isrc -o build/test_plugin tests/test_plugin.c /mv/wrapper/vst2_wrap.c \
      src/engine.c src/seq_core.c src/theory.c src/gen.c src/melody.c src/voicing.c src/rhythm.c src/bassline.c src/melody2.c src/jamlib.c src/presets.c -lm -ldl -lpthread
  ./build/test_plugin
  gcc -std=gnu11 -Wall -Wextra -Werror $SAN -Isrc -o build/core_parity tests/core_parity.c src/seq_core.c src/theory.c src/gen.c src/melody.c src/voicing.c src/rhythm.c src/jamlib.c src/bassline.c src/melody2.c -lm -lpthread
  ./build/core_parity > build/core_parity_native.txt
  ./build/core_parity | cmp - build/core_parity_native.txt || { echo "run_tests: FAIL: the core parity trace differs between two runs" >&2; exit 1; }
'
sha=$( (sha256sum build/core_parity_native.txt 2>/dev/null || shasum -a 256 build/core_parity_native.txt) | cut -d' ' -f1)
want=$(cut -d' ' -f1 tests/core_parity.sha256)
if [ "$sha" != "$want" ]; then
  echo "run_tests: core parity got  $sha"; echo "run_tests: core parity want $want"
  echo "run_tests: FAIL: the core's behaviour changed (update tests/core_parity.sha256 only for an intended change)"
  exit 1
fi
echo "core parity: $(grep -c '^[0-9]* 9[0-9a-f] [0-9]* [1-9]' build/core_parity_native.txt) notes at 44.1 kHz and 48 kHz, as pinned"
"$MPC_VST/tools/test_port.sh" vst/vst.json
