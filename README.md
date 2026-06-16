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
| Sound source | **Synth** (built-in voices, no samples) or **Samples** (Salamander WAV) |
| Adaptive density | **Follow** mode maps the energy of the input/guide track to density |
| BPM sync | Reads host transport; falls back to own BPM parameter |
| Sample engine | Salamander Drumkit (WAV), stereo mix |
| State persistence | Full APVTS XML save/restore |
| Formats | VST3, Standalone |

---

## UI overview

```
┌─────────────────────────────────────────────────────┐
│  ADAPTIVE DRUMMER                          [ Play ] │
├─────────────────────────────────────────────────────┤
│  Style   [ Rock ]  [ Jazz ]  [ Electronic ]         │
│  Density [ Sparse ] [ Medium ] [ Full ]             │
│  Follow  [ Follow ]   ENERGY ▕███████░░░░░░░▏        │
│  Sound   [ Synth ] [ Samples ]                      │
├─────────────────────────────────────────────────────┤
│  BPM  120.0  (synced to host)                       │
│                                                     │
│  Volume ◎                                           │
├─────────────────────────────────────────────────────┤
│  Samples: kick/ snare/ hihat/ crash/ ride/ tom/     │
│  [Load samples...]                                  │
└─────────────────────────────────────────────────────┘
         420 × 384 px
```

- **Style row** — radio buttons (group 1); selects the groove vocabulary.
- **Play / Stop** (top-right) — transport toggle. In the standalone it starts/stops the drummer; in a plugin it also plays while the host transport is stopped.
- **Density row** — radio buttons (group 2); controls how many hits are placed per bar. Disabled while **Follow** is on (density is then automatic).
- **Follow toggle** — when on, the density tracks the energy of the track feeding the effect instead of the manual density buttons.
- **Energy meter** — live 0–1 guide energy, refreshed at 10 Hz; drives the adaptive density.
- **Sound** — choose **Synth** (built-in drum voices, no samples) or **Samples** (Salamander WAV).
- **BPM field** — shows the live tempo (refreshed at 10 Hz). **Double-click to type a tempo** (40–240) — this is how you set the speed in the Standalone. When a DAW transport supplies the tempo the field locks and follows the host.
- **Volume knob** — rotary (drag up/down or left/right), range 0–1, default 0.8.
- **Load samples…** — opens a folder chooser; expects the Salamander layout described below.

---

## Host compatibility

Adaptive Drummer is built as an **audio effect / generator** (VST3 category `Fx|Instrument`),
so it loads anywhere effects do — including **Adobe Audition's Effects Rack**, and on audio
tracks in Reaper, Ableton Live, Cubase, FL Studio, Bitwig — or run the **Standalone** app and
press **Play**.

Insert it on a track: it **outputs the generated drums**. With **Follow** on, the track's
incoming audio is the guide (louder/busier → fuller pattern). Put it on a dedicated track for a
pure drum machine, or on a part you want it to react to for adaptive density.

> Some Adobe hosts have a known VST3 quirk where the custom editor may not render (you get
> generic parameter sliders). It still loads and plays.

**Packaging (per the VST3 spec).** A VST3 is a **bundle — a folder, not a single file**; the
single-file `.vst3` DLL was *deprecated in VST 3.6.10*. The real binary lives at
`Adaptive Drummer.vst3/Contents/<arch>/Adaptive Drummer.vst3` (`x86_64-win` on Windows)
alongside `Contents/Resources/moduleinfo.json`. Copy the whole `Adaptive Drummer.vst3` folder
into your VST3 path (`%COMMONPROGRAMFILES%\VST3` on Windows) and rescan — the "several files in
a nested folder" layout is correct and required.


## Parameters (APVTS)

| ID | Type | Range | Default | Description |
|---|---|---|---|---|
| `style` | Choice | 0 Rock · 1 Jazz · 2 Electronic | 0 | Groove style |
| `density` | Choice | 0 Sparse · 1 Medium · 2 Full | 1 | Pattern density |
| `bpm` | Float | 40–240 (step 0.1) | 120 | Tempo; **editable in the UI** (double-click the BPM field). Host tempo overrides it when present |
| `volume` | Float | 0–1 (step 0.01) | 0.8 | Output gain |
| `follow` | Bool | off · on | off | Adaptive density from the guide track (overrides `density`) |
| `source` | Choice | 0 Synth · 1 Samples | 0 | Sound source: synthesised voices or WAV samples |
| `play` | Bool | off · on | off | Transport: generate drums (also driven by the host transport) |

All parameters are automatable and saved with the DAW session.

---

## Sound sources

The drummer can play through one of two engines, switched with the **Sound**
buttons (the `source` parameter):

- **Synth** *(default)* — built-in procedural drum voices (kick, snare, hi-hat,
  crash, ride, tom). Needs no sample files, so the plugin makes sound the moment
  it loads — nothing to download.
- **Samples** — plays the Salamander Drumkit WAVs (see [Samples](#samples)). Use
  this for the realistic acoustic kit; load a folder once and it is remembered.

Style, density and Follow behave the same in both modes.

---

## Adaptive "Follow" mode

This is a generator effect, so its **audio input is the guide track**. With **Follow**
enabled, the incoming guide audio is analysed every block and its energy drives
the pattern density automatically:

- block RMS -> dB -> normalised to 0-1 -> attack/release envelope -> **energy**;
- energy maps to **Sparse / Medium / Full** through a hysteresis band, so the
  density rises and falls smoothly instead of chattering at the thresholds.

Route the part you want the drummer to react to (a vocal, a guitar, a full mix)
into the track that feeds this effect and turn **Follow** on. Louder/busier guide ->
fuller pattern; quieter guide -> sparser pattern. With Follow off, the input
is ignored and the manual **Density** buttons are used. The `style` stays manual
in both modes.

> Put the effect on the track whose audio should steer the drums. When the input
> is silent the energy falls to zero (Sparse).

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

## Building on Linux / running the tests

The plugin also builds on Linux (used by CI). Install the JUCE build
dependencies, then configure and build:

```sh
sudo apt install libfreetype-dev libfontconfig1-dev libx11-dev libxext-dev \
  libxinerama-dev libxrandr-dev libxcursor-dev libxcomposite-dev \
  libasound2-dev libgl1-mesa-dev

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

The unit-test target (`AdaptiveDrummerTests`, on by default) runs via CTest:

```sh
ctest --test-dir build --output-on-failure
```

To skip building the tests, configure with `-DADAPTIVE_DRUMMER_BUILD_TESTS=OFF`.
CI (`.github/workflows/ci.yml`) runs on every push: it builds the plugin and
tests on **Linux and Windows**, runs the unit tests, and validates the built
VST3 with [pluginval](https://github.com/Tracktion/pluginval) (strictness 10).

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
that follows the layout above. The chosen folder is **remembered in the plugin
state**, so it is reloaded automatically when you reopen the session or the
plugin — you only need to pick it once.

---

## Project structure

```
Adaptive-drumming-vst\
├── CMakeLists.txt                    # JUCE plugin target + unit-test target
├── .gitmodules                       # JUCE submodule
├── .github\workflows\ci.yml          # Linux build + tests (CI)
├── assets\
│   └── samples\salamander\           # Salamander Drumkit (not in repo)
├── src\
│   ├── PluginProcessor.h/.cpp        # APVTS, host BPM sync, transport, state I/O
│   ├── PluginEditor.h/.cpp           # 420×340 UI (incl. Follow + energy meter)
│   └── drummer\
│       ├── AdaptiveDrummer.h/.cpp    # Orchestrator
│       ├── DrumPattern.h/.cpp        # 16-step bitmask grid
│       ├── DrumSampler.h/.cpp        # WAV loader + playback
│       └── EnergyAnalyzer.h/.cpp     # Guide-track energy → adaptive density
├── tests\                            # JUCE UnitTest suite (run via CTest)
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
