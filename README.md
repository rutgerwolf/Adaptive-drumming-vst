# Adaptive Drummer

A JUCE-based adaptive drum machine — an audio **effect/generator** plugin
(VST3 + Standalone), spun off from the
[WinBand](https://github.com/rutgerwolf/winband) project.

Adaptive Drummer generates rhythmic drum patterns that match a playing style and
intensity/complexity you choose. It syncs to the host DAW's BPM automatically, or runs at its
own tempo when used as a standalone application.

---

## Features

| Feature | Details |
|---|---|
| Styles | Rock, Jazz, Electronic |
| Groove axes | **Intensity** (dynamics) × **Complexity** (how busy) — continuous 0–1 sliders |
| Sound source | **Synth** (built-in voices, no samples) or **Samples** (Salamander WAV) |
| Adaptive groove | **Follow** mode maps the energy of the input/guide track onto Intensity/Complexity |
| BPM / tempo | Reads host transport; **editable BPM** (double-click) otherwise |
| Sample engine | Salamander Drumkit (WAV), stereo mix |
| State persistence | Full APVTS XML save/restore |
| Formats | VST3, Standalone |

---

## UI overview

```
┌─────────────────────────────────────────────────────┐
│  ADAPTIVE DRUMMER                          [ Play ] │
├─────────────────────────────────────────────────────┤
│  Style      [ Rock ]  [ Jazz ]  [ Electronic ]      │
│  Intensity  ▬▬▬▬▬▬●▬▬▬▬  0.55                       │
│  Complexity ▬▬▬▬▬▬●▬▬▬▬  0.55                       │
│  Follow  [ Follow ]   ENERGY ▕███████░░░░░░░▏        │
│  Sound   [ Synth ] [ Samples ]                      │
├─────────────────────────────────────────────────────┤
│  BPM  120.0  (type to set)                          │
│                                                     │
│  Volume ◎                                           │
├─────────────────────────────────────────────────────┤
│  Samples: kick/ snare/ hihat/ crash/ ride/ tom/     │
│  [Load samples...]                                  │
└─────────────────────────────────────────────────────┘
         420 × 416 px
```

- **Style row** — radio buttons (group 1); selects the groove vocabulary.
- **Play / Stop** (top-right) — transport toggle. In the standalone it starts/stops the drummer; in a plugin it also plays while the host transport is stopped.
- **Intensity / Complexity sliders** — the 2-D groove axes: Intensity is per-hit dynamics, Complexity is how busy the pattern is. Continuous 0–1, replacing the old 3-step density buttons (a visual XY pad is a later UI pass). Disabled and shown live while **Follow** is on (then they're automatic).
- **Follow toggle** — when on, Intensity/Complexity track the energy of the track feeding the effect instead of the manual sliders.
- **Energy meter** — live 0–1 guide energy, refreshed at 10 Hz; drives the adaptive groove.
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
incoming audio is the guide (louder/busier → higher intensity and complexity). Put it on a
dedicated track for a pure drum machine, or on a part you want it to react to for an adaptive
groove.

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
| `intensity` | Float | 0–1 (step 0.01) | 0.55 | Dynamics axis — per-hit velocity (used when Follow is off) |
| `complexity` | Float | 0–1 (step 0.01) | 0.55 | Structural axis — how busy the pattern is (used when Follow is off) |
| `density` | Choice | 0 Sparse · 1 Medium · 2 Full | 1 | Legacy; kept registered for parameter-ID stability. No longer read by `processBlock()`, only used to migrate a pre-2.0 saved session's `intensity`/`complexity` |
| `bpm` | Float | 40–240 (step 0.1) | 120 | Tempo; **editable in the UI** (double-click the BPM field). Host tempo overrides it when present |
| `volume` | Float | 0–1 (step 0.01) | 0.8 | Output gain |
| `follow` | Bool | off · on | off | Adaptive intensity/complexity from the guide track (overrides the sliders) |
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

Style, Intensity/Complexity and Follow behave the same in both modes.

---

## Adaptive "Follow" mode

This is a generator effect, so its **audio input is the guide track**. With **Follow**
enabled, the incoming guide audio is analysed every block and its energy drives
Intensity and Complexity automatically:

- block RMS -> dB -> normalised to 0-1 -> attack/release envelope -> **energy**;
- energy maps to a **Sparse / Medium / Full** step through a hysteresis band (so it
  rises and falls smoothly instead of chattering at the thresholds), which is then
  mapped onto the continuous **Intensity/Complexity** axes — the same mapping the
  legacy `density` parameter used, so the three points land on exactly the same feel.

Route the part you want the drummer to react to (a vocal, a guitar, a full mix)
into the track that feeds this effect and turn **Follow** on. Louder/busier guide ->
higher intensity and complexity; quieter guide -> sparser. With Follow off, the input
is ignored and the manual **Intensity/Complexity** sliders are used directly. The
`style` stays manual in both modes.

> Put the effect on the track whose audio should steer the drums. When the input
> is silent the energy falls to zero (Sparse).

---

## Drum patterns

Patterns are 16-step (16th-note) grids. Voices: Kick, Snare, HiHat, Crash, Ride, Tom.

| Style | Low complexity | Medium | Full complexity |
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
| VST3 plugin (a bundle **folder**) | `build\AdaptiveDrummer_artefacts\Release\VST3\Adaptive Drummer.vst3\` |
| Standalone app | `build\AdaptiveDrummer_artefacts\Release\Standalone\Adaptive Drummer.exe` |

### Step 5 — Install the VST3

Copy the whole `Adaptive Drummer.vst3` **folder** (it is a bundle, not a single
file) to your DAW’s VST3 scan folder:

```bat
xcopy /E /I "build\AdaptiveDrummer_artefacts\Release\VST3\Adaptive Drummer.vst3" "%COMMONPROGRAMFILES%\VST3\Adaptive Drummer.vst3"
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
CI (`.github/workflows/ci.yml`) runs on pushes to `main` and on pull requests:
it builds on **Linux and Windows**, runs the unit tests, and validates the built
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
├── .github\workflows\ci.yml          # CI: Linux + Windows build, tests, pluginval
├── assets\
│   └── samples\salamander\           # Salamander Drumkit (not in repo)
├── src\
│   ├── PluginProcessor.h/.cpp        # APVTS, host BPM sync, transport, state I/O
│   ├── PluginEditor.h/.cpp           # 420×416 UI (incl. Follow + energy meter)
│   └── drummer\
│       ├── AdaptiveDrummer.h/.cpp    # Orchestrator
│       ├── DrumPattern.h/.cpp        # GrooveTable: 2D intensity x complexity model
│       ├── DrumStepClock.h           # Sample-accurate step timing
│       ├── GrooveHash.h              # Stateless seeded hashing for the groove model
│       ├── DrumSampler.h/.cpp        # WAV loader + playback
│       ├── DrumSynth.h/.cpp          # Built-in procedural drum voices
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
