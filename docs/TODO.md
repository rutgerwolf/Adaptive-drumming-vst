# Adaptive Drummer — Next-Phase TODO

> **Current focus: get the product running and usable in real hosts.**
> Musicality work (fills, humanisation, more styles) comes *after* the
> run-and-distribute basics are solid. Detailed engineering notes for many of
> these items live in [`ROADMAP.md`](ROADMAP.md).

## Phase A — Get it running (priority)

- [ ] Smoke-test the **Standalone** on Windows: launch, **Play**, double-click **BPM** to set tempo, toggle **Synth / Samples**, move **Volume**.
- [ ] Smoke-test the **VST3** in a DAW (Reaper / Live / Cubase): insert on a track, confirm the host transport plays the drums and the tempo follows the host.
- [ ] Verify in **Adobe Audition's Effects Rack**: it appears in the effect list, loads, and plays.
- [ ] Note whether the **custom editor renders** in Audition or falls back to generic sliders. If it falls back, investigate the known JUCE/Adobe VST3 editor quirk and document a workaround.
- [ ] Confirm **Follow** mode reacts to real audio on the track (energy meter moves, density changes).
- [ ] Sanity-check **state save/restore** in a real session (reopen → settings + sample folder remembered).

## Phase B — Build & distribute

- [ ] Have CI upload **release artifacts** (the `Adaptive Drummer.vst3` bundle + the Standalone) per OS.
- [ ] Cut a **GitHub Release** with those artifacts and copy-paste install steps (copy the whole `.vst3` *folder* into the VST3 path).
- [ ] (Optional) **macOS** build + codesign + notarize path.
- [ ] (Optional) Tiny installer / zip with a README for non-technical users.

## Phase C — Musicality (Phase 4)

- [ ] **A2** — rebuild the pattern only when `style`/`density` changes (groundwork; avoids rebuilding every block).
- [ ] **D1** — per-step velocity & accents.
- [ ] **Fills / turnarounds** at the end of a bar or every N bars.
- [ ] **Humanisation** — subtle timing/velocity jitter.
- [ ] More styles and per-style variations.
- [ ] **MIDI-output mode** — emit MIDI notes (instead of / alongside audio) so it can drive other drum instruments.

## Known issues / cleanups (from ROADMAP)

- [ ] **A1 / A3** — audio-thread review items (see `ROADMAP.md`).
- [ ] **B4** — integer-truncation tempo drift over very long runs.
- [ ] Sample swap under a lock to avoid a race when loading a new kit while playing.
