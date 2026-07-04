#pragma once
#include "DrumPattern.h"

#include <atomic>

/**
 * TableExchange — preallocated, lock-free SPSC 2-slot mailbox for handing a
 * dynamically-built GrooveTable from a non-audio producer to the audio
 * thread (ARCHITECTURE_2D_FOLLOW.md §2.5, roadmap Step 19).
 *
 * This is the "future dynamic tables" half of §2.5: built-in styles keep
 * using the Step 11 const-pointer path (DrumPattern::loadStyle) unchanged.
 * TableExchange only exists so a *later* feature (per-voice mute toggles,
 * user-edited grids, generated fills — anything built off the audio thread)
 * has somewhere RT-safe to publish into. This PR wires the mailbox all the
 * way into AdaptiveDrummer's bar-wrap machinery but ships with **no
 * production producer** — nothing ever calls tryStage()/publish() yet, so it
 * is provably inert: see TableExchangeTest.cpp for the protocol tests and
 * AdaptiveDrummerTest.cpp's determinism test for the behavioural proof.
 *
 * ── Roles (never overlap) ───────────────────────────────────────────────
 *   Producer — message thread or a ThreadPool job. Builds a table off the
 *   audio thread (may allocate, block, touch juce::String — none of that
 *   matters here since it isn't RT) and hands it over with tryStage() /
 *   publish(). Exactly one producer at a time (SPSC, not MPSC).
 *   Consumer — the audio thread, and only at the step-0 bar wrap
 *   (AdaptiveDrummer::processBlock). Adopts the newest table with consume()
 *   and can hand it back with releaseActive().
 *
 * ── Ownership invariant ─────────────────────────────────────────────────
 * Each of the two slots has exactly one owner at any instant:
 *   - the producer, while building into a slot returned by tryStage() (not
 *     yet published — no atomic points at it);
 *   - pendingSlot, once published and not yet adopted;
 *   - activeSlot, once adopted by the consumer and not yet released.
 * tryStage() only ever hands out a slot that is neither pendingSlot nor
 * activeSlot, so the producer can never write into a slot either atomic
 * currently names. That is the entire safety argument; everything below is
 * about how the two atomics change hands without a lock.
 *
 * ── Why tryStage() reads pendingSlot before activeSlot ──────────────────
 * consume()'s successful path is: store activeSlot (relaxed), then
 * compare_exchange_strong pendingSlot from s to -1 with
 * memory_order_acq_rel/acquire. The *release* half of that CAS is what
 * publishes the new activeSlot value to other threads — a thread that
 * observes pendingSlot == -1 via an acquire load is guaranteed (by the
 * acquire/release pairing) to also see whichever activeSlot store happened
 * before that release. So tryStage() must load pendingSlot first, with
 * acquire, and only then load activeSlot: if it read activeSlot first, a
 * consume() could complete between the two loads and tryStage() could
 * observe a *stale* activeSlot (the just-vacated slot) as still "active",
 * which is merely overly conservative — but reading them in the other order
 * risks the reverse mistake, seeing pendingSlot == -1 without the
 * happens-before edge to activeSlot's fresh value, which is not just
 * conservative, it is unsafe (a slot the consumer just claimed could look
 * free). Reading pendingSlot first, with acquire, closes that.
 *
 * ── Why consume() stores activeSlot before the claiming CAS ─────────────
 * Publishing intent (activeSlot = s) *before* the CAS that actually claims
 * pendingSlot means there is a brief window where activeSlot names a slot
 * that pendingSlot has not yet released. That window is deliberately
 * conservative, never unsafe: during it, tryStage() sees slot s excluded
 * twice over (once as the stale pendingSlot, once as the new activeSlot) —
 * at worst a producer's tryStage() transiently returns nullptr when it could
 * technically have proceeded. The alternative ordering (CAS first, then
 * store activeSlot) would open a real race: after the CAS clears
 * pendingSlot but before activeSlot is set, tryStage() would see neither
 * atomic naming slot s and could hand it straight back to the producer —
 * which would then scribble over the table the audio thread is about to
 * dereference as its just-adopted "active" table. Store-before-CAS trades a
 * harmless transient double-occupancy for closing that real one.
 *
 * ── Lock-free, not wait-free ─────────────────────────────────────────────
 * Both slots can legitimately be spoken for at once (one active, one
 * pending) — in that state tryStage() returns nullptr. A producer must
 * treat nullptr as "coalesce this update into the next attempt and retry
 * later" (e.g. on the next message-thread timer tick), never as a signal to
 * spin: the state resolves within one bar while the transport is running,
 * because every bar wrap calls consume(), which frees the previously-active
 * slot's ownership of pendingSlot (though not activeSlot — see
 * releaseActive() below). A stopped transport never reaches a bar wrap, so
 * it never calls consume() either; a producer publishing while the
 * transport is stopped will simply see its table adopted once playback
 * resumes and the next wrap occurs (or coalesced into a subsequent publish
 * if the producer moves on first) — there is no deadline.
 *
 * ── releaseActive() ──────────────────────────────────────────────────────
 * consume() claims a slot into activeSlot and leaves it there indefinitely —
 * that slot stays unavailable to the producer until something explicitly
 * frees it. AdaptiveDrummer calls releaseActive() when it stops using the
 * adopted table for a reason other than adopting a newer one (e.g. a style
 * flip repoints DrumPattern's active table back to a const style table), so
 * the slot doesn't sit permanently unstageable.
 *
 * ── Why 2 slots, not 3 ────────────────────────────────────────────────────
 * A third slot would make this wait-free triple-buffering (tryStage() could
 * never transiently fail even with one active + one pending + one being
 * staged) at a cost of one more preallocated GrooveTable (+576 B). Not worth
 * it while there is no production producer at all — deliberately deferred
 * until an actual dynamic-table feature needs the extra headroom.
 */
class TableExchange
{
public:
    static_assert (std::atomic<int>::is_always_lock_free,
                   "TableExchange requires a lock-free std::atomic<int> — it is used on the audio thread");

    // ── Producer side (message thread or ThreadPool job; never the audio thread) ──

    /** Slot to build the next table into, or nullptr if both slots are
        currently held (one active, one pending) — retry after the next bar
        wrap; that state resolves within one bar while the transport runs. */
    GrooveTable* tryStage() noexcept
    {
        // MUST read pendingSlot first (see class comment: this ordering is
        // what gives the activeSlot load below its happens-before guarantee
        // via consume()'s release-CAS).
        const int p = pendingSlot.load (std::memory_order_acquire);
        const int a = activeSlot.load  (std::memory_order_acquire);
        for (int i = 0; i < 2; ++i)
            if (i != p && i != a) { stagedIdx = i; return &slots[i]; }
        return nullptr;
    }

    /** Publishes the slot returned by the last tryStage(). Must be fully
        written first. Last-writer-wins: an unconsumed earlier publish is
        superseded. */
    void publish() noexcept
    {
        pendingSlot.store (stagedIdx, std::memory_order_release);
    }

    // ── Consumer side (audio thread, at the step-0 bar wrap only) ──────────

    /** The newest published table, or nullptr if nothing new. Claims the slot:
        the producer cannot restage it until the consumer adopts a later one
        (or releaseActive() is called). */
    const GrooveTable* consume() noexcept
    {
        int s = pendingSlot.load (std::memory_order_acquire);
        while (s >= 0)
        {
            // Publish intent BEFORE claiming: closes the reclaim race where a
            // producer could otherwise compute this slot as free and scribble
            // over the table this thread is about to adopt (see class comment).
            activeSlot.store (s, std::memory_order_relaxed);
            if (pendingSlot.compare_exchange_strong (s, -1,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return &slots[s];
            // CAS reloaded s with a newer publish — adopt that one instead.
        }
        return nullptr;
    }

    /** Consumer stopped using its adopted slot (e.g. a style flip repointed
        to a const style table). Frees the slot for the producer. */
    void releaseActive() noexcept
    {
        activeSlot.store (-1, std::memory_order_relaxed);
    }

private:
    GrooveTable      slots[2];            // preallocated, ~1.2 KB total
    std::atomic<int> pendingSlot { -1 };  // -1 = nothing published
    std::atomic<int> activeSlot  { -1 };  // -1 = consumer holds nothing
    int              stagedIdx   { -1 };  // producer-thread only
};
