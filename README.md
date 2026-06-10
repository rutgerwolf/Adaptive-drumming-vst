# Adaptive Drummer

A JUCE-based adaptive drum machine plugin (VST3 + Standalone), spun off from the [WinBand](https://github.com/rutgerwolf/winband) project.

## Features

- Three styles: Rock, Jazz, Electronic
- Three pattern densities: Sparse, Medium, Full
- Host BPM sync (falls back to its own BPM parameter when the host provides none)
- Salamander Drumkit sample playback

## Platform

| | |
|---|---|
| Formats | VST3, Standalone |
| Build system | CMake >= 3.22 |
| Compiler | MSVC 2022 or Clang/LLVM (C++17) |

## Building

```bash
git clone --recurse-submodules https://github.com/rutgerwolf/Adaptive-drumming-vst.git
cd Adaptive-drumming-vst
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Output: `build/AdaptiveDrummer_artefacts/VST3/AdaptiveDrummer.vst3`

## Samples

The Salamander Drumkit samples are not included in this repository.

1. Download from <https://archive.org/details/SalamanderDrumkit> (CC BY 3.0, Alexander Holm)
2. Place the WAV files in `assets/samples/salamander/` using the layout described in
   [assets/samples/salamander/.gitkeep](assets/samples/salamander/.gitkeep):
   `kick/ snare/ hihat/ crash/ ride/ tom/`

The plugin loads samples automatically from `assets/samples/salamander` next to the binary, or lets you pick a folder via the "Load samples..." button.

## Project structure

```
Adaptive-drumming-vst/
├── CMakeLists.txt
├── assets/
│   └── samples/salamander/      # Salamander Drumkit (CC BY 3.0, not in repo)
├── src/
│   ├── PluginProcessor.h/.cpp   # APVTS parameters, host sync, state
│   ├── PluginEditor.h/.cpp      # 420x300 UI: style, density, BPM, volume
│   └── drummer/                 # DrumPattern, DrumSampler, AdaptiveDrummer
└── third_party/JUCE             # git submodule
```

## Third-party credits

| Component | Licence |
|---|---|
| [JUCE](https://juce.com) | GPL v3 (or JUCE commercial licence) |
| Salamander Drumkit — Alexander Holm | CC BY 3.0 |

## Licence

The source code in this repository is released under the terms in [LICENSE](LICENSE).
Note that builds link against JUCE, which carries its own licence terms (see above).
