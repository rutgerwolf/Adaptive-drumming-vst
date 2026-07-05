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
| Groove axes | **Intensity** (dynamics) × **Complexity** (how busy) — one draggable XY pad, continuous 0–1 |
| Sound source | **Synth** (built-in voices, no samples) or **Samples** (Salamander WAV) |
| Adaptive groove | **Follow** mode layers guide-driven accents + intensity on top of the grid (which never overrides it) — route in *any* track via the Sidechain input, or fall back to this effect's own input; strength set by **Follow Amount** |
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
│                                                     │
│  GROOVE                                            │
│  ┌───────────────────────┐                         │
│  │ INTENSITY             │                         │
│  │            ●          │                         │
│  │                       │                         │
│  │            COMPLEXITY │                         │
│  └───────────────────────┘                         │
│                                                     │
│  Follow  [ Follow ]   ENERGY ▕███████░░░░░░░▏        │
│                                                     │
│  FOLLOW AMT                                        │
│  ▬▬▬▬▬▬▬▬▬▬●▬▬▬▬▬▬▬▬▬                              │
│  Sound   [ Synth ] [ Samples ]                      │
├─────────────────────────────────────────────────────┤
│  BPM  120.0  (type to set)          Volume ◎        │
├─────────────────────────────────────────────────────┤
│  [Load samples...]      Samples loaded              │
└─────────────────────────────────────────────────────┘
         420 × 618 px
```

- **Style row** — radio buttons (group 1); selects the groove vocabulary.
- **Play / Stop** (top-right) — transport toggle. In the standalone it starts/stops the drummer; in a plugin it also plays while the host transport is stopped.
- **Groove XY pad** — the 2-D groove axes in one draggable pad: X is Complexity (how busy), Y is Intensity (per-hit dynamics), matching GarageBand Drummer's own X/Y grid. Drag anywhere in the pad to set both at once; double-click resets to the default (0.55, 0.55). **Always stays enabled and draggable, including while Follow is on** — the grid is always authoritative; Follow's live modulation shows as a small ring overlay instead of moving the pad.
- **Follow toggle** — when on, a connected guide track adds accents and shifts intensity up/down *on top of* the grid — it never changes the grid's complexity or replaces its intensity outright. A different track routed into the Sidechain input if connected, otherwise this effect's own input.
- **Follow Amount** — how strongly Follow's modulation applies, 0 (off) to 1 (strongest); always automatable, even while Follow itself is off.
- **Energy meter** — live 0–1 guide energy, refreshed at 10 Hz; feeds the adaptive modulation.
- **Sound** — choose **Synth** (built-in drum voices, no samples) or **Samples** (Salamander WAV).
- **BPM field** — shows the live tempo (refreshed at 10 Hz). **Double-click to type a tempo** (40–240) — this is how you set the speed in the Standalone. When a DAW transport supplies the tempo the field locks and follows the host.
- **Volume knob** — rotary (drag up/down or left/right), range 0–1, default 0.8.
- **Load samples…** — opens a folder chooser; point it at an unmodified Salamander Drumkit download (auto-recognised, see [Samples](#samples)) or a manually-sorted folder for other packs.

---

## Host compatibility

Adaptive Drummer is built as an **audio effect / generator** (VST3 category `Fx|Instrument`),
so it loads anywhere effects do — including **Adobe Audition's Effects Rack**, and on audio
tracks in Reaper, Ableton Live, Cubase, FL Studio, Bitwig — or run the **Standalone** app and
press **Play**.

Insert it on a track: it **outputs the generated drums**. With **Follow** on, it reacts
to a guide signal (accents on transients, louder → more intense) on top of whatever
groove the XY pad is set to — ideally a *different* track routed in via your host's
sidechain input (see
[Adaptive "Follow" mode](#adaptive-follow-mode) for per-DAW routing), or this track's
own input as a fallback. Put it on a dedicated track for a pure drum machine, or on a
part you want it to react to for an adaptive groove.

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
| `intensity` | Float | 0–1 (step 0.01) | 0.55 | Dynamics axis — per-hit velocity. Always the grid's base value; Follow modulates *around* it, never replaces it |
| `complexity` | Float | 0–1 (step 0.01) | 0.55 | Structural axis — how busy the pattern is. **Always** the grid's value — Follow never touches it, on or off |
| `density` | Choice | 0 Sparse · 1 Medium · 2 Full | 1 | Legacy; kept registered for parameter-ID stability. No longer read by `processBlock()`, only used to migrate a pre-2.0 saved session's `intensity`/`complexity` |
| `bpm` | Float | 40–240 (step 0.1) | 120 | Tempo; **editable in the UI** (double-click the BPM field). Host tempo overrides it when present |
| `volume` | Float | 0–1 (step 0.01) | 0.8 | Output gain |
| `follow` | Bool | off · on | off | Guide-driven accent + intensity modulation layered on top of the grid (see [Adaptive "Follow" mode](#adaptive-follow-mode)) |
| `followAmount` | Float | 0–1 (step 0.01) | 0.6 | How strongly Follow's modulation applies; 0 = full bypass |
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

With **Follow** enabled, a guide signal is analysed every block, but it never
overrides the XY pad — the grid always sets the base groove (which cells fire is
always the grid's `complexity`; nothing else can change that). Follow instead
layers two bounded, velocity-only modulations on top of it:

- **Accents** — a fast/slow envelope pair on the guide's level detects transient
  moments (louder *than the guide's own recent level*, not an absolute threshold).
  A detected accent nudges the velocity of whatever the pattern is playing at that
  instant, weighted by role: the authored emphasis hits (kick 1, backbeats, crash)
  swing the most, ghost notes are untouched, everything else leans in modestly.
- **Intensity contour** — a slower envelope of the guide's phrase-level loudness
  pulls the effective intensity toward itself, blended around the grid's `intensity`
  value. The grid always keeps at least 25% authority, even at maximum Follow
  strength, so a loud or silent guide shifts the feel without ever fully taking over.

**Follow Amount** (0–1, default 0.6) scales both modulations together — 0 is a full
bypass (Follow behaves exactly as if it were off), higher values lean harder on the
guide. This mirrors GarageBand/Logic Drummer's own Follow-strength slider, which
also keeps its X/Y pad authoritative and only lets the followed track shape a bounded
part of the groove.

**The guide is a track of your choice**, not necessarily this effect's own input:

- **Route a different track in (recommended)** — the plugin exposes a stereo
  **Sidechain** input bus, enabled by default (some hosts' sidechain-routing UI only
  lets you route into a bus the plugin already reports as active). Route the part you
  want the drummer to react to (a vocal, a guitar, a full mix) into it via your host's
  sidechain/send routing, and it takes priority as the guide the moment it's connected.
  - **Adobe Audition:** on the source track's own **S1** send, pick
    **Side-Chain → Create → Adaptive Drummer**. Route the source track directly —
    confirmed in testing that chaining through an intermediate bus (source → Bus A →
    Bus A's own send → Sidechain) does *not* reliably carry audio through in
    Audition, even though the routing looks correct in the mixer and in the
    plugin's own channel-routing dialog. A direct track → Sidechain send does work.
  - **Reaper / Ableton Live / Cubase / Studio One / FL Studio / Bitwig:** use the
    host's normal sidechain-input picker on the plugin (exact wording varies by host).
- **No sidechain connected** — falls back to analysing this effect's own input (put
  it on the track you want it to react to). **Standalone** has no "other track", so
  it always uses this fallback.

With Follow off, or on with no guide connected at all (nothing routed in, and the
main input disabled), Follow modulates nothing — the manual **Intensity/Complexity**
XY pad is used exactly as set, a plain passthrough. The `style` stays manual in both
modes.

> A guide that *is* connected but silent still modulates — intensity eases down and
> accents stop, the same way a drummer plays quieter when the band drops out.

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
2. Extract it — the download is a **flat, SFZ-mapped kit**, not sorted into
   per-instrument folders. Files are named like `kick_OH_F_1.wav`,
   `snare_OH_MF_1.wav`, `hihatClosed_OH_F_1.wav`, and so on, all at (roughly)
   one folder level.
3. Click **Load samples…** in the plugin UI and point it straight at the
   extracted folder — **no manual sorting needed.**

Loading works via a small shipped manifest (`assets/salamander_manifest.json`)
that maps each of the plugin's 6 voices (kick/snare/hihat/crash/ride/tom) to a
priority list of the real Salamander filenames. One recursive scan of the
folder you pick matches those names, however deep they sit — so the
unmodified download just works.

The chosen folder is **remembered in the plugin state**, so it is reloaded
automatically when you reopen the session or the plugin — you only need to
pick it once.

**Other sample packs.** If a pack doesn't match the manifest, any voice it
can't resolve falls back to the original folder-per-voice layout — this is
still fully supported, per voice (not all-or-nothing):

```
<your folder>\
├── kick\          one or more WAV files; the first one found is used
├── snare\
├── hihat\
├── crash\
├── ride\
└── tom\
```

A voice with neither a manifest match nor a matching subfolder just stays
silent — the rest of the kit still loads and plays.

---

## Project structure

```
Adaptive-drumming-vst\
├── CMakeLists.txt                    # JUCE plugin target + unit-test target
├── .gitmodules                       # JUCE submodule
├── .github\workflows\ci.yml          # CI: Linux + Windows build, tests, pluginval
├── assets\
│   ├── salamander_manifest.json      # Maps voices -> real Salamander filenames
│   └── samples\salamander\           # Salamander Drumkit (not in repo)
├── src\
│   ├── PluginProcessor.h/.cpp        # APVTS, host BPM sync, transport, state I/O
│   ├── PluginEditor.h/.cpp           # 420×618 UI (incl. Follow + energy meter)
│   ├── XYPad.h/.cpp                  # Draggable 2-D groove pad (Complexity x Intensity)
│   └── drummer\
│       ├── AdaptiveDrummer.h/.cpp    # Orchestrator
│       ├── DrumPattern.h/.cpp        # GrooveTable: 2D intensity x complexity model
│       ├── DrumStepClock.h           # Sample-accurate step timing
│       ├── GrooveHash.h              # Stateless seeded hashing for the groove model
│       ├── DrumSampler.h/.cpp        # WAV loader + playback
│       ├── DrumSynth.h/.cpp          # Built-in procedural drum voices
│       └── EnergyAnalyzer.h/.cpp     # Guide-track energy -> Follow's accent/intensity modulation
├── docs\                             # Architecture/roadmap history (see below)
├── tests\                            # JUCE UnitTest suite (run via CTest)
└── third_party\
    └── JUCE\                         # git submodule
```

`docs\` traces *why* the code looks the way it does, across two design passes:

| File | What it covers |
|---|---|
| `FABLE_HANDOFF.md` / `FABLE_PROMPT.md` | Original context pack + design brief for the 2-D groove engine |
| `ARCHITECTURE_2D_FOLLOW.md` | Chosen architecture: GrooveTable model, sidechain bus, params/state migration |
| `FABLE_REVIEW.md` | Confirmed findings from that review, mapped to roadmap steps |
| `ROADMAP_2D.md` | The 19-step implementation plan (steps 1–13 shipped; step 14's Follow-mapping plan is superseded — see below) |
| `ARCHITECTURE_FOLLOW_V2.md` | Replaces `ARCHITECTURE_2D_FOLLOW.md` §3.4 / `ROADMAP_2D.md` step 14: the shipped Follow design (grid-preserving accent + intensity modulation, steps 20–21) |
| `BPM_HOST_TEMPO_FEASIBILITY.md` | Research behind the tap-tempo counter: why it sets the plugin's own manual BPM instead of writing the host's tempo (no standard plugin format allows that; REAPER is the sole documented exception) |
| `ROADMAP.md`, `TODO.md` | Earlier-phase planning notes |

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
