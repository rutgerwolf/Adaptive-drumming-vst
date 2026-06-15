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
- **BPM display** — read-only label, refreshed at 10 Hz from the processor. Shows host BPM when a DAW transport is active.
- **Volume knob** — rotary (drag up/down or left/right), range 0–1, default 0.8.
- **Load samples…** — opens a folder chooser; expects the Salamander layout described below.

---

## Parameters (APVTS)

| ID | Type | Range | Default | Description |
|---|---|---|---|---|
| `style` | Choice | 0 Rock · 1 Jazz · 2 Electronic | 0 | Groove style |
| `density` | Choice | 0 Sparse · 1 Medium · 2 Full | 1 | Pattern density |
| `bpm` | Float | 40–240 (step 0.1) | 120 | Fallback BPM (ignored when host provides one) |
| `volume` | Float | 0–1 (step 0.01) | 0.8 | Output gain |

All parameters are automatable and saved with the DAW session.

---

## Drum patterns

Patterns are 16-step (16th-note) grids. Voices: Kick, Snare, HiHat, Crash, Ride, Tom.

| Style | Sparse | Medium | Full |
|---|---|---|---|
| Rock | Four-on-the-floor + backbeat | + open hi-hat | + fills |
| Jazz | Ride pulse + rim | + brush hi-hat | + ride variations |
| Electronic | Kick 1+3 | + clap + 16th hi-hat | + synth-tom accents |

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

## Building on Windows

### Prerequisites

| Tool | Download | Notes |
|---|---|---|
| Git for Windows | <https://git-scm.com/download/win> | Required for submodule support |
| CMake 3.22+ | <https://cmake.org/download/> | Tick “Add to PATH” during install |
| Visual Studio 2022 | <https://visualstudio.microsoft.com/> | Workload: **Desktop development with C++** |

### Step 1 — Clone (including JUCE submodule)

```bat
git clone --recurse-submodules https://github.com/rutgerwolf/Adaptive-drumming-vst.git
cd Adaptive-drumming-vst
```

If you already cloned without `--recurse-submodules`:

```bat
git submodule update --init --recursive
```

### Step 2 — Configure

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
```

### Step 3 — Build

```bat
cmake --build build --config Release
```

### Step 4 — Output

| Artifact | Path |
|---|---|
| VST3 plugin | `build\AdaptiveDrummer_artefacts\Release\VST3\AdaptiveDrummer.vst3` |
| Standalone app | `build\AdaptiveDrummer_artefacts\Release\Standalone\AdaptiveDrummer.exe` |

### Step 5 — Install the VST3

Copy `AdaptiveDrummer.vst3` to your DAW’s VST3 scan folder:

```bat
xcopy /E /I build\AdaptiveDrummer_artefacts\Release\VST3\AdaptiveDrummer.vst3 "%COMMONPROGRAMFILES%\VST3\AdaptiveDrummer.vst3"
```

Then rescan plugins in your DAW.

---

## Samples

The Salamander Drumkit WAV files are **not included** in this repository (~2 GB, CC BY 3.0).

1. Download from <https://archive.org/details/SalamanderDrumkit>.
2. Extract and place the WAV files in `assets\samples\salamander\` next to the
   cloned repo, using this layout:

```
salamander\
├── kick\          e.g. kick_OH_F_1.wav, kick_OH_MP_1.wav …
├── snare\
├── hihat\
├── crash\
├── ride\
└── tom\
```

Alternatively, click **Load samples…** in the plugin UI and point it at any folder
that follows the layout above.

---

## Project structure

```
Adaptive-drumming-vst\
├── CMakeLists.txt                    # JUCE plugin target (VST3 + Standalone)
├── .gitmodules                       # JUCE submodule
├── assets\
│   └── samples\salamander\           # Salamander Drumkit (not in repo)
├── src\
│   ├── PluginProcessor.h/.cpp        # APVTS, host BPM sync, state I/O
│   ├── PluginEditor.h/.cpp           # 420×300 UI
│   └── drummer\
│       ├── AdaptiveDrummer.h/.cpp    # Orchestrator
│       ├── DrumPattern.h/.cpp        # 16-step bitmask grid
│       └── DrumSampler.h/.cpp        # WAV loader + playback
└── third_party\
    └── JUCE\                         # git submodule
```

---

## Troubleshooting

| Problem | Cause | Fix |
|---|---|---|
| `third_party\JUCE` is empty | Cloned without `--recurse-submodules` | `git submodule update --init --recursive` |
| Build error: `juce_audio_processors not found` | Same as above | Same fix |
| Plugin silent after build | Salamander WAVs not present | See [Samples](#samples) section |
| Plugin not found by DAW | VST3 not in scan path | Copy to `%COMMONPROGRAMFILES%\VST3\` and rescan |
| `CMake Error: JUCE_MODULES_DIR` | Stale CMake cache | Delete `build\` and reconfigure |

---

## Third-party credits

| Component | Licence |
|---|---|
| [JUCE](https://juce.com) | GPL v3 / JUCE commercial licence |
| Salamander Drumkit — Alexander Holm | CC BY 3.0 |

## Licence

The source code in this repository is released under the terms in [LICENSE](LICENSE).
Builds link against JUCE, which carries its own licence terms (see above).
