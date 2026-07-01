# Fable 5 assignment — deep review + architecture & roadmap

> Paste this as the opening instruction to **Claude Fable 5** in a Claude Code
> session on this repository. Read [`FABLE_HANDOFF.md`](FABLE_HANDOFF.md) first —
> it has all the context so you do not have to rediscover it.

---

## Role & goal

You are the senior architect for **Adaptive Drummer**, a JUCE VST3/Standalone drum
plugin. Your job is **one high-value pass**: a rigorous, real-time-safety-aware
review of the codebase, followed by an architecture and a **dependency-ordered
roadmap** to evolve the plugin toward a GarageBand-Drummer-style product:

1. style choice, 2. a continuous **2-D intensity × complexity** grid, 3. **follow a
user-chosen other track**.

Everything you need is in `FABLE_HANDOFF.md` (product goal, current signal flow, the
`DrumPattern` 2-D gap at `DrumPattern.h:49`, known issues B1–D1, strawman designs,
open questions, constraints). Treat the strawmen as material to improve or discard.

## Scope

- **In scope:** whole-codebase review (correctness + audio-thread/RT-safety +
  architecture + musicality + threading/state + test coverage); the target
  architecture for the three features; a phased implementation roadmap.
- **Out of scope / non-goals:** do **not** implement the features now, and do **not**
  propose a wholesale rewrite. Evolve the existing code. The roadmap must be
  executable by a cheaper model (Opus 4.8) step by step.

## Deliverables (write these files)

1. **`docs/FABLE_REVIEW.md`** — verified findings, most-severe first. Each: location
   (`file:line`), concrete failure scenario, RT-safety impact, fix sketch. Confirm or
   refute the handoff's B2/B4/A2/D1 and find what it missed.
2. **`docs/ARCHITECTURE_2D_FOLLOW.md`** — the design for: (a) the 2-D groove engine
   (per-step/per-voice velocity model; how intensity & complexity map to the pattern;
   where generation runs and how the swap is RT-safe); (b) follow-another-track
   (sidechain bus, per-DAW routing, analysis→axis mapping, no-sidechain fallback);
   (c) a recommendation on audio-first vs MIDI-out-first. Resolve the six open
   questions in the handoff. Include the parameter/state-migration plan for
   `density → intensity + complexity`.
3. **`docs/ROADMAP_2D.md`** — dependency-ordered steps. Each step: goal · files
   touched · RT-safety note · test to add · risk · rough size. Ordered so CI stays
   green throughout (Linux + Windows build, CTest, pluginval strictness 10).

## Suggested agent orchestration

You are built for long-horizon multi-agent work — use it, but keep it **bounded**
(this is the expensive pass; a cheaper model did the prep and will do the
implementation). A good shape (e.g. via the Workflow tool):

- **Phase 1 — Review fan-out** (parallel, one agent per dimension): ① audio-thread /
  RT-safety ② JUCE/VST3 correctness ③ DSP & musicality ④ threading/state/lifecycle
  ⑤ test coverage. Each returns structured findings.
- **Phase 2 — Adversarial verify** (parallel, per finding): a skeptic that tries to
  refute each finding; keep only what survives. (Your differentiated strength is
  sustained, structured verification — lean on it here.)
- **Phase 3 — Design panel** (parallel): 2–3 independent designs each for the 2-D
  engine and for follow-another-track → judge → synthesize the winner, grafting the
  best ideas from runners-up.
- **Phase 4 — Planner/synthesis** (single agent): produce the three deliverable docs
  from the verified review + chosen design.

## Constraints

- **RT-safety is non-negotiable:** no allocation/locks/IO on the audio thread; name
  the audio-thread implications of every change. Reuse the C1 safe-swap pattern for
  any dynamic pattern regeneration.
- **Keep it building on CI at every roadmap step.** Don't propose a step that leaves
  `main` red.
- **Cost control:** run this as a single bounded pass. Prefer `effort: high` on the
  verify/design/synthesis stages and a lighter effort on mechanical fan-out reads;
  cap the number of agents. Don't re-explore what `FABLE_HANDOFF.md` already states.

## Output

End with a short executive summary in chat: the top 3–5 verified issues, the chosen
2-D and follow designs in a paragraph each, and the first 3 roadmap steps you'd hand
to Opus. The three `docs/*.md` files are the durable deliverable.
</content>
