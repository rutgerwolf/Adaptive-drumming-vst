# BPM_HOST_TEMPO_FEASIBILITY — can a plugin write the host DAW's own tempo?

> **Status.** Research only — nothing in this document is implemented. The shipped feature is the tap-tempo counter (the `TAP` button next to the BPM display, `src/TapTempo.h`), which computes a BPM estimate and sets the plugin's own manual `"bpm"` parameter — it does **not** write the host's tempo. This document is the feasibility investigation behind that decision: whether writing the host's own tempo is possible at all, and if so, where. All claims below were verified against primary sources (actual SDK headers — including the AAX SDK vendored in this repo's JUCE submodule — the CLAP repo, and Steinberg's `vst3_pluginterfaces` repo), not secondhand summaries.

## Executive verdict

**There is no standard, cross-DAW way for an audio plugin to change the host's tempo, and there will not be one in the foreseeable future.** VST3, AU (v2 and v3), and AAX are all strictly read-only for tempo — verified against the actual current interface headers, including every host-facing callback interface in the VST3 SDK. CLAP *does* have a plugin→host `request_tempo()` call in its `transport-control` extension, but it has sat in `draft/` since CLAP 1.0 (2022) and **no shipping host could be confirmed to implement it** — so today it is a spec, not a feature. The **only route with solid evidence is REAPER-only**: REAPER hands plugins its full C API (including `SetCurrentBPM`) through a proprietary VST3 host interface (`IReaperHostApplication`), and JUCE 8.0.13 (the version this repo uses) already has first-class hooks for exactly this (`AudioProcessor::getVST3ClientExtensions()` + a shipped REAPER example). If this is ever built, it should be a small, clearly-labeled, REAPER-only tempo write-back, on top of the tap-tempo counter's existing manual-BPM path everywhere else. A tempo-write feature marketed as general-purpose is not honestly buildable.

---

## 1. VST3 (current Steinberg SDK) — **Not possible.** Confidence: high.

Verified against the live `steinbergmedia/vst3_pluginterfaces` repo (master), the interface layer of `vst3sdk`:

- **Full directory audit**: the complete `pluginterfaces/vst/` listing (31 headers: `ivstaudioprocessor.h`, `ivsteditcontroller.h`, `ivstprocesscontext.h`, `ivstcontextmenu.h`, `ivstdataexchange.h`, `ivsthostapplication.h`, `ivstinterappaudio.h`, `ivstremapparamid.h`, etc.) contains no interface that lets a plugin write tempo or transport state.
- **`ivstprocesscontext.h`**: `ProcessContext::tempo` is a plain `double` field the host fills per block ("tempo in BPM … (optional)", gated by `kTempoValid`). It is a one-way information container; there is no "propose new tempo" counterpart anywhere.
- **`ivsteditcontroller.h`** — every host-side (plugin-callable) interface enumerated: `IComponentHandler` (`beginEdit/performEdit/endEdit/restartComponent`), `IComponentHandler2` (`setDirty/requestOpenEditor/startGroupEdit/finishGroupEdit`), `IComponentHandlerBusActivation`, `IProgress`, `IComponentHandlerSystemTime` (wall-clock only). Zero tempo/transport methods. `IEditControllerHostEditing` is the reverse direction (host informing the plugin of host-driven edits) and irrelevant. `IComponentHandler3` is just `createContextMenu` (`ivstcontextmenu.h`) — UI menus, not transport.
- **`ivstinterappaudio.h`** (the only header with any host-control surface): iOS Inter-App Audio only, and its `IInterAppAudioHost` offers screen size, icons, preset browsing, and `showSettingsView()` — no programmatic tempo/transport control. Deprecated platform anyway.

The design intent is explicit in the docs: "For each processing block the host provides timing information" — unidirectional. Sources: [pluginterfaces/vst directory](https://github.com/steinbergmedia/vst3_pluginterfaces/tree/master/vst), [ivstprocesscontext.h](https://github.com/steinbergmedia/vst3_pluginterfaces/blob/master/vst/ivstprocesscontext.h), [ivsteditcontroller.h](https://github.com/steinbergmedia/vst3_pluginterfaces/blob/master/vst/ivsteditcontroller.h), [VST3 dev portal](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Index.html).

## 2. AU (v2 and v3) — **Not possible.** Confidence: high.

- **AUv2**: `kAudioUnitProperty_HostCallbacks` supplies a `HostCallbackInfo` struct of *getter* function pointers the plugin may call — `HostCallback_GetBeatAndTempo`, `HostCallback_GetMusicalTimeLocation`, `HostCallback_GetTransportState(2)`. All are queries; there is no setter counterpart in AudioToolbox.
- **AUv3**: the equivalents are `AUAudioUnit.musicalContextBlock` (`AUHostMusicalContextBlock` — returns `currentTempo`, beat position, etc.) and `transportStateBlock` (`AUHostTransportStateBlock`). Both are blocks *the host optionally provides for the plugin to read* (GarageBand famously doesn't even set them). AUv3 added nothing in the write direction; Apple developer forum threads on AUv3 tempo are exclusively about *getting* tempo, with no set mechanism existing ([thread 46085](https://developer.apple.com/forums/thread/46085), [thread 69230](https://developer.apple.com/forums/thread/69230)).
- iOS Inter-App Audio "remote control events" (play/rewind/record) existed but never included tempo, and the whole IAA framework is deprecated.

## 3. AAX — **Not possible.** Confidence: high (verified from the actual SDK headers).

The AAX SDK is vendored in this repo at `third_party/JUCE/modules/juce_audio_plugin_client/AAX/SDK/Interfaces/`, so this was checked against the real headers, not summaries:

- `AAX_ITransport.h`: `GetCurrentTempo(double*)` and `GetCurrentMeter(...)` are **const getters**. The complete method list contains no tempo/meter setter of any kind.
- `AAX_IACFTransportControl.h` (the newest transport-control addition): exactly two methods — `RequestTransportStart()` and `RequestTransportStop()`. So AAX plugins *can* ask Pro Tools to start/stop playback, but **cannot touch tempo**.
- Nothing else in the Interfaces directory matches `SetTempo`/`SetCurrentTempo`/`RequestTempo` (grep across all headers: zero hits). Control surfaces (EUCON) can change Pro Tools tempo, but that is a separate product category, not a hosted audio plugin.

## 4. CLAP — **Possible on paper, not in practice today.** Confidence: high on the spec; medium-high on "no host support" (closed-source hosts can't be exhaustively audited, but no positive evidence exists anywhere).

**The spec finding (the headline item):** CLAP's [`include/clap/ext/draft/transport-control.h`](https://github.com/free-audio/clap/blob/main/include/clap/ext/draft/transport-control.h) defines `clap_host_transport_control` (extension ID `"clap.transport-control/2"`), and it includes exactly what this feature needs:

```c
void(CLAP_ABI *request_tempo)(const clap_host_t *host, double tempo);
```

plus `request_start/stop/continue/pause/toggle_play/jump/loop_region/record/time_signature`. The header itself warns: "The host has no obligation to execute these requests, so the interface may be partially working."

**The adoption reality (this is what kills it for now):**
- It is still in the `draft/` directory (as of this writing), where it has been since CLAP 1.0 shipped in June 2022. Draft extensions are explicitly unstable and hosts rarely ship them.
- **No host could be confirmed to implement the host side.** Checked: the official reference host [free-audio/clap-host](https://github.com/free-audio/clap-host) (sets `process.transport = nullptr`, implements params/gui/state/etc. — no transport-control); Qtractor's CLAP host (`qtractorClapPlugin.cpp` — full `get_extension()` branch list has no transport-control); MultitrackStudio ([documents](https://www.multitrackstudio.com/clapplugin.php) supporting "almost all extensions except draft ones", and its short draft-exception list does *not* include transport-control); Bitwig and REAPER — no documentation, changelog entry, forum post, or plugin anywhere claims either honors `clap.transport-control`. The free-audio issue tracker's only transport-control item is a constant-name fix (#89).
- Additional practical blockers for this repo: JUCE has no official CLAP plugin target (this repo's JUCE 8.0.13 builds VST3/AU/AAX/LV2/Standalone only — verified in `juce_audio_plugin_client/`); a CLAP build would require the community `clap-juce-extensions` shim, all to reach an extension no host runs.

**Bottom line:** CLAP is the right long-term watch item — if Bitwig or REAPER ever ships `clap.transport-control`, this becomes real. Today it is not actionable.

## 5. Host-specific extensions beyond REAPER — **REAPER is the only true in-plugin route; everything else is architecturally a separate program.** Confidence: high for REAPER/Studio One/FL/Ardour (primary sources); medium-high for the rest.

- **REAPER (the positive finding):** REAPER exposes its entire C API to hosted plugins. For **VST3**, the host context passed at initialization implements `IReaperHostApplication` (`reaper_vst3_interfaces.h`); calling `getReaperApi("SetCurrentBPM")` yields `void SetCurrentBPM(ReaProject*, double bpm, bool wantUndo)`. Crucially, **JUCE 8.0.13 in this repo already has the plumbing**: `AudioProcessor::getVST3ClientExtensions()` (declared in `juce_AudioProcessor.h`) → `VST3ClientExtensions::setIHostApplication(Steinberg::FUnknown*)`, and JUCE ships a worked example doing exactly this query at `third_party/JUCE/examples/Plugins/ReaperEmbeddedViewPluginDemo.h` (~lines 211–220, querying `IReaperHostApplication::iid` and calling `getReaperApi(...)`). No wrapper hacking needed. REAPER also hands the same API to CLAP plugins via `host->get_extension("cockos.reaper_extension")` ([Cockos forum](https://forums.cockos.com/showthread.php?t=285121)) — irrelevant here since the VST3 route is simpler.
- **Ableton Live:** the Live API (`live_set tempo`) is writable — but only from Max for Live devices or MIDI Remote Scripts. An M4L device is a Max patch hosted by Live's Max runtime; it is architecturally **not** the VST plugin, and a VST3 plugin has no path to the Live API. A *companion* M4L device (plugin talks to it over localhost OSC/UDP, device sets `live_set tempo`) is technically workable but is a second product the user must install, and requires Live Suite / an M4L license.
- **Bitwig:** the Controller API (`Transport.tempo().set(...)`) is writable — from controller-extension scripts (Java/JS) installed separately in Bitwig, not from plugins. Same companion-process architecture as M4L. Bitwig exposes no plugin-facing tempo write in either VST3 or (as far as any evidence shows) CLAP.
- **Cubase/Nuendo:** Steinberg gives its own host nothing beyond standard VST3 for third parties (they *own* the VST3 spec — see §1). No vendor extension exists.
- **Studio One:** PreSonus publishes real VST3 host extensions ([fenderdigital/presonus-plugin-extensions](https://github.com/fenderdigital/presonus-plugin-extensions)); verified `ipslcontextinfo.h` directly — plugin-writable attributes are channel name/color/selection/volume/pan/mute/solo/send levels only. **No tempo attribute exists at all** in the interface.
- **FL Studio:** the native FL plugin SDK's plugin→host surface (dispatcher messages, `on_message`/`midi_out`/hints — verified via the [fpsdk](https://github.com/tonikasoft/fpsdk) port mirroring `fp_plugclass.h`) has tempo only as a **host→plugin notification** (`Event::Tempo`). No setter. Standard VST3 hosting adds nothing.
- **Ardour:** open source; hosts VST3/AU/LV2 with the PreSonus extensions vendored (`libs/vst3/pslextensions/` — no tempo, per above) and no REAPER-style API bridge. Ardour's Lua scripting and OSC surface can edit the tempo map, but those are user-installed control surfaces, not the plugin.
- **Logic Pro:** AU only; nothing beyond standard AU (§2). No plugin-facing scripting surface exists.

## 6. Indirect workarounds (MIDI Clock / MTC) — **Possible in 2–3 DAWs as a fragile power-user trick; never seamless.** Confidence: medium-high.

- **MTC is a non-starter for tempo**: MIDI Time Code carries wall-clock position (SMPTE) only — it contains no tempo information, so a DAW slaved to MTC chases position, not BPM.
- **MIDI Clock (0xF8 at 24 PPQN)** does encode tempo. But the plugin cannot deliver it through the host: VST3's event system (`ivstevents.h` event types + `LegacyMIDICCOutEvent`) **cannot represent MIDI system-realtime messages at all**, so a VST3 plugin cannot emit clock through its MIDI output bus. The practical route is the plugin opening an **OS-level virtual MIDI port directly** (`juce::MidiOutput` → macOS IAC bus / Windows loopMIDI) from inside plugin code, bypassing the host.
- **Which DAWs can slave their own master tempo to incoming MIDI clock**: Ableton Live (yes — enable "Sync" on a MIDI input port; tempo follows, but Live enters EXT mode, disabling its own tempo automation) and REAPER (yes — external timecode sync supports MIDI clock/SPP). Bitwig has limited clock-receive support. **Cannot**: Logic Pro (sends clock, never slaves tempo to it), Pro Tools (MTC only; Beat Clock is send-only), Cubase/Nuendo (no MIDI-clock slave), FL Studio (long-standing gap — see e.g. [Gearspace: "DAW that will BPM sync to MIDI clock"](https://gearspace.com/board/music-computers/1363252-daw-will-bpm-sync-midi-clock-but-not-transport-sync.html)).
- **Assessment**: even in Live/REAPER the user must create a virtual MIDI port, enable sync on it, and accept clock-jitter tempo wobble and loss of tempo automation. This can never be packaged as a one-click feature, and for REAPER specifically the native API (§5) is strictly better. Not recommended as a product feature; at most a documentation footnote for expert users.

## 7. JUCE — **No abstraction for tempo write; confirmed in this repo's JUCE and current upstream.** Confidence: high.

- This repo's `third_party/JUCE` is **JUCE 8.0.13** (from `juce_StandardHeader.h`: `JUCE_MAJOR_VERSION 8 / MINOR 0 / BUILDNUMBER 13`) — effectively current.
- `modules/juce_audio_basics/audio_play_head/juce_AudioPlayHead.h`: `getPosition()` is the read path. `PositionInfo::setBpm()` exists but is just a struct setter used by *hosts/wrappers filling in* the info — it does not write anything back. The header *does* declare `canControlTransport()` / `transportPlay()` / `transportRecord()` / `transportRewind()` — transport, **not tempo** — and the defaults are no-ops with `canControlTransport()` returning false. Grep across all JUCE modules shows the **only** non-default implementation is iOS Inter-App Audio (`juce_audio_devices/native/juce_Audio_ios.cpp`); none of the VST3/AU/AAX/LV2/Standalone plugin wrappers implement it. So even JUCE's existing transport-control surface is dead code for desktop plugins, and no tempo equivalent exists at all.
- What JUCE *does* provide (and is the enabler for the REAPER route): per-format client extensions — `VST3ClientExtensions` (with `setIHostApplication()` and `queryIEditController()`), `VST2ClientExtensions`, `AAXClientExtensions` in `juce_audio_processors*/utilities/`, hooked via `AudioProcessor::getVST3ClientExtensions()`, plus the shipped `ReaperEmbeddedViewPluginDemo.h` example demonstrating the exact `IReaperHostApplication`/`getReaperApi()` handshake.
- JUCE has **no CLAP plugin-client** (verified: `juce_audio_plugin_client` contains AAX/AU/AUv3/LV2/Standalone/Unity/VST2/VST3 only); CLAP builds require the third-party `clap-juce-extensions`.

---

## If this is ever built: smallest viable scope

**Recommended scope: REAPER-only tempo write-back (VST3), runtime-detected.**

1. In `AdaptiveDrummerProcessor` (`src/PluginProcessor.cpp`), add a `juce::VST3ClientExtensions` implementation returned from `getVST3ClientExtensions()`. In `setIHostApplication(FUnknown*)`, query `IReaperHostApplication` (vendor header `reaper_vst3_interfaces.h`, ~100 lines, same as JUCE's example uses). If present, cache `SetCurrentBPM = (void(*)(ReaProject*, double, bool)) reaperHost->getReaperApi("SetCurrentBPM")` (`nullptr` for the project targets the active project).
2. Runtime detection, not a compile guard: the same binary works everywhere; `SetCurrentBPM != nullptr` simply means "we're in REAPER". No `#if` needed.
3. UI: the tap-tempo control would get a small "→ set REAPER tempo" toggle/button, visible only when the REAPER API was acquired, labeled as REAPER-specific. Everywhere else, tap-tempo continues to drive the existing manual `bpm` parameter.
4. Call `SetCurrentBPM` from the message thread (it edits project state / undo history — never from `processBlock`); pass `wantUndo = true` so the user can undo.
5. Explicitly out of scope, per the evidence above: AU/AAX variants (impossible), a CLAP build targeting `transport-control` (no host runs it — revisit if Bitwig/REAPER ever ship the extension), MIDI-clock loopback (fragile, worse than the native API in the only overlapping host), and companion M4L/Bitwig-extension products (separate installables, disproportionate effort for a tap-tempo nicety).

If REAPER-only is judged not worth it, the honest alternative — and the one currently shipped — is: host tempo stays read-only, the tap-tempo counter drives the manual `bpm` fallback, and the user copies the tapped number into their DAW's own tempo field by hand.
