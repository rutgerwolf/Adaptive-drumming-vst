# Adaptive Drummer

A JUCE-based adaptive drum machine plugin (VST3 + Standalone), spun off from the
[WinBand](https://github.com/rutgerwolf/winband) project.

Adaptive Drummer generates rhythmic drum patterns that match a playing style and
density you select. It syncs to the host DAW's BPM automatically, or runs at its
own tempo when used as a standalone application.

---

## Features

| Feature | Details |
|---|---|
| Styles | Rock, Jazz, Electronic |
| Pattern densities | Sparse, Medium, Full |
| BPM sync | Reads host transport; falls back to own BPM parameter |
| Sample engine | Salamander Drumkit (WAV), stereo mix |
| State persistence | Full APVTS XML save/restore |
| Formats | VST3, Standalone |

---

## UI overview

```
┌─────────────────────────────────────────────────────┐
│  ADAPTIVE DRUMMER                                   │
├─────────────────────────────────────────────────────┤
│  Style   [ Rock ]  [ Jazz ]  [ Electronic ]         │
│  Density [ Sparse ] [ Medium ] [ Full ]             │
├─────────────────────────────────────────────────────┤
│  BPM  120.0  (synced to host)                       │
│                                                     │
│  Volume ◎                                           │
├─────────────────────────────────────────────────────┤
│  Samples: kick/ snare/ hihat/ crash/ ride/ tom/     │
│  [Load samples...]                                  │
└─────────────────────────────────────────────────────┘
         420 × 300 px
```

- **Style row** — radio buttons (group 1); selects the groove vocabulary.
- **Density row** — radio buttons (group 2); controls how many hits are placed per bar.
- **BPM display** — read-only label, refreshed at 10 Hz from the processor. Shows host
  BPM when a DAW transport is active.
- **Volume knob** — rotary (drag up/down or left/right), range 0 – 1, default 0.8.
- **Load samples…** — opens a folder chooser; expects the Salamander layout below.

---

## Parameters (APVTS)

| ID | Type | Range | Default | Description |
|---|---|---|---|---|
| `style` | Choice | 0 Rock · 1 Jazz · 2 Electronic | 0 | Groove style |
| `density` | Choice | 0 Sparse · 1 Medium · 2 Full | 1 | Pattern density |
| `bpm` | Float | 40 – 240 (step 0.1) | 120 | Fallback BPM (ignored when host provides one) |
| `volume` | Float | 0 – 1 (step 0.01) | 0.8 | Output gain |

All parameters are automatable and saved with the DAW session.

---

## Drum patterns

Patterns are 16-step (16th-note) grids at three densities. Voices: Kick, Snare,
HiHat, Crash, Ride, Tom.

| Style | Sparse | Medium | Full |
|---|---|---|---|
| Rock | Four-on-the-floor + backbeat | + open hihat | + fills |
| Jazz | Ride pulse + rim | + brush hi-hat | + ride variations |
| Electronic | Kick 1+3 | + clap + 16th hihat | + synth-tom accents |

---

## Platform

| | |
|---|---|
| OS | Windows 10/11, macOS 11+ |
| Formats | VST3, Standalone |
| Build system | CMake ≥ 3.22 |
| Compiler | MSVC 2022 or Clang/LLVM (C++17) |
| JUCE | git submodule (master branch) |

---

## Building

```bash
git clone --recurse-submodules https://github.com/rutgerwolf/Adaptive-drumming-vst.git
cd Adaptive-drumming-vst
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Output: `build/AdaptiveDrummer_artefacts/VST3/AdaptiveDrummer.vst3`

> On macOS, add `-DCMAKE_OSX_ARCHITECTURES=arm64` (Apple Silicon) or `x86_64` as
> needed. AU format requires Xcode and is not enabled by default in the current
> CMakeLists; add `AU` to `FORMATS` to enable it.

---

## Samples

The Salamander Drumkit WAV files are **not included** in this repository (size ~2 GB,
licence CC BY 3.0).

### Download

1. Go to <https://archive.org/details/SalamanderDrumkit>
2. Download and extract the archive.

### Layout expected under `assets/samples/salamander/`

```
salamander/
├── kick/          # e.g. kick_OH_F_1.wav, kick_OH_MP_1.wav …
├── snare/
├── hihat/
├── crash/
├── ride/
└── tom/
```

Each sub-folder should contain WAV files named with velocity layers. The sampler
picks the file whose name contains the closest dynamic marker (`PP`, `MP`, `MF`,
`F`, `FF`). Any WAV files present are loaded; missing folders are silently skipped
(that voice will be silent).

Alternatively, click **Load samples…** in the plugin UI and point it at any folder
that follows the layout above.

---

## Project structure

```
Adaptive-drumming-vst/
├── CMakeLists.txt                    # JUCE plugin target (VST3 + Standalone)
├── .gitmodules                       # JUCE submodule
├── assets/
│   └── samples/salamander/           # Salamander Drumkit (not in repo)
│       └── .gitkeep                  # Documents expected layout
├── src/
│   ├── PluginProcessor.h/.cpp        # APVTS, host BPM sync, state I/O
│   ├── PluginEditor.h/.cpp           # 420×300 UI
│   └── drummer/
│       ├── AdaptiveDrummer.h/.cpp    # Orchestrator; selects pattern + triggers sampler
│       ├── DrumPattern.h/.cpp        # 16-step bitmask grid, 3 styles × 3 densities
│       └── DrumSampler.h/.cpp        # Salamander WAV loader + per-voice playback
└── third_party/
    └── JUCE/                         # git submodule → github.com/juce-framework/JUCE
```

---

## Troubleshooting

| Problem | Cause | Fix |
|---|---|---|
| Submodule folder `third_party/JUCE` is empty | Cloned without `--recurse-submodules` | Run `git submodule update --init --recursive` |
| Plugin silent, no samples loaded | WAV files not present | See [Samples](#samples) section above |
| Plugin not found by DAW after build | VST3 not in scan path | Copy `AdaptiveDrummer.vst3` to your DAW's VST3 folder (`C:\Program Files\Common Files\VST3` on Windows) |
| Build error: `juce_audio_processors not found` | JUCE submodule not initialised | Same as above: `git submodule update --init --recursive` |
| `CMake Error: JUCE_MODULES_DIR` | Old CMake cache | Delete `build/` and reconfigure |

---

## Third-party credits

| Component | Licence |
|---|---|
| [JUCE](https://juce.com) | GPL v3 / JUCE commercial licence |
| Salamander Drumkit — Alexander Holm | CC BY 3.0 |

## Licence

The source code in this repository is released under the terms in [LICENSE](LICENSE).
Builds link against JUCE, which carries its own licence terms (see above).
