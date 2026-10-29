# Credits

Cadenza is built on, and learns from, MIT-licensed work by sd88me and contributors:

- **[mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins)** (MIT, Copyright (c) 2026 sd88me): the build
  toolchain, the generic VST2 wrapper (`wrapper/vst2_wrap.c`) this plugin links against, the skin builder, and
  the hardware notes (`docs/NOTES.md`, `docs/MIDI_TIMING.md`) the timing and MIDI design follow. Used from a
  separate checkout at build time; not copied into this repo.
- **[mpc-vst-euclidier](https://github.com/sd88me/mpc-vst-euclidier)** (MIT, Copyright (c) 2026 sd88me): the
  reference for a multi-lane MIDI sequencer on the Force (lane rows, a playhead drawn as a cell state, a channel
  per lane).
- **Maschine Group** (in mpc-vst-plugins, `ports/maschine`): the dlopen-based ALSA sequencer port in
  `src/engine.c` follows its approach.

No code from GPL-licensed plugins (for example mpc-vst-acid) is used.
