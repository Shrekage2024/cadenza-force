# Cadenza

A chord-driven generative MIDI sequencer: a native VST2 plugin for the Akai Force. Pick a key and scale, shape
chords, bass and melody with a few knobs per page, and Cadenza generates and follows along as you play.

This repo holds the sequencer engine and the Force plugin's source. Cadenza also has a browser version and an
iOS app; those are developed separately and aren't part of this repo.

## Building

Needs a checkout of [sd88me/mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) (the build toolchain and
generic VST2 wrapper this plugin links against) next to this repo, and Docker.

```
vst/build.sh
```

produces `vst/build/cadenza.so`, the skin, and the plugin-list entry, then checks the skin and binary size
against `vst/budget.json`.

## Testing

```
tests/run_tests.sh
```

runs the engine's offline tests (core, theory, generator, rhythm, bassline, melody), the plugin through the
toolchain's real VST2 wrapper, and the toolchain's own host test, all under ASan/UBSan. Needs `vst/build.sh`
first. Must end with `PASSED`.

## Layout

- `src/` – the sequencer engine: song position → steps → notes (`seq_core.c`), keys/scales/chord-relative tones
  (`theory.c`), the chord, bass and melody generators, and the 52 named voicing styles. No host or ALSA code.
- `src/engine.c` – plugin glue for the toolchain's wrapper: skin parameters, cell labels, the ALSA MIDI port.
- `vst/` – plugin config, skin generator and build script.
- `tests/` – offline tests.

## Licence

[PolyForm Shield 1.0.0](LICENSE) — free to use, study, modify and distribute for any purpose except building a
product that competes with Cadenza. See [CREDITS.md](CREDITS.md) for third-party licences.
