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
 * ── Why every operation on both atomics is sequentially consistent ──────
 * An earlier version of this class used acquire/release on pendingSlot and
 * a *relaxed* store for activeSlot, reasoning that observing pendingSlot
 * == -1 via an acquire load (synchronizing with consume()'s release-CAS)
 * would transitively guarantee visibility of the activeSlot store that
 * preceded it in program order. That reasoning is unsound in practice: nothing
 * pins *which* release a given load synchronizes with when the same value
 * (-1) is written by many different CAS calls over time, and a producer's two
 * loads (pendingSlot then activeSlot) are not required to observe a
 * consistent snapshot across *different* atomics under acquire/release alone
 * — one load can be "fresher" than the other. In concentrated stress testing
 * this manifested as real, reproducible torn reads (a consumer adopting a
 * slot mid-write). Fixed by making every operation on both atomics
 * memory_order_seq_cst: all threads agree on a single global total order of
 * these operations, so if a producer's read of pendingSlot is ordered after
 * a given consume() call in that total order, its very next read of
 * activeSlot is *also* guaranteed ordered no earlier than that same call —
 * eliminating any possibility of a stale view of one atomic paired with a
 * fresh view of the other. This mailbox is consumed at most once per bar
 * (not per sample) and published at most a handful of times per second, so
 * seq_cst's cost (a full memory fence per operation, vs. acquire/release) is
 * immaterial here; the simplicity of a single, easily-verified ordering
 * model is worth far more than the saved cycles would be.
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
 * harmless transient double-occupancy for closing that real one. (This
 * ordering requirement is independent of the seq_cst change above — it holds
 * regardless of memory order, since it is about *program order* on the
 * consumer thread, not cross-thread visibility.)
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
        // seq_cst on both loads (see class comment): guarantees a fresh view
        // of activeSlot whenever pendingSlot is observed as caught up with a
        // given consume() call, which acquire/release alone did not.
        const int p = pendingSlot.load (std::memory_order_seq_cst);
        const int a = activeSlot.load  (std::memory_order_seq_cst);
        for (int i = 0; i < 2; ++i)
            if (i != p && i != a) { stagedIdx = i; return &slots[i]; }
        return nullptr;
    }

    /** Publishes the slot returned by the last tryStage(). Must be fully
        written first. Last-writer-wins: an unconsumed earlier publish is
        superseded. */
    void publish() noexcept
    {
        pendingSlot.store (stagedIdx, std::memory_order_seq_cst);
    }

    // ── Consumer side (audio thread, at the step-0 bar wrap only) ──────────

    /** The newest published table, or nullptr if nothing new. Claims the slot:
        the producer cannot restage it until the consumer adopts a later one
        (or releaseActive() is called). */
    const GrooveTable* consume() noexcept
    {
        int s = pendingSlot.load (std::memory_order_seq_cst);
        while (s >= 0)
        {
            // Publish intent BEFORE claiming: closes the reclaim race where a
            // producer could otherwise compute this slot as free and scribble
            // over the table this thread is about to adopt (see class comment).
            activeSlot.store (s, std::memory_order_seq_cst);
            if (pendingSlot.compare_exchange_strong (s, -1,
                    std::memory_order_seq_cst, std::memory_order_seq_cst))
                return &slots[s];
            // CAS reloaded s with a newer publish — adopt that one instead.
        }
        return nullptr;
    }

    /** Consumer stopped using its adopted slot (e.g. a style flip repointed
        to a const style table). Frees the slot for the producer. */
    void releaseActive() noexcept
    {
        activeSlot.store (-1, std::memory_order_seq_cst);
    }

private:
    GrooveTable      slots[2];            // preallocated, ~1.2 KB total
    std::atomic<int> pendingSlot { -1 };  // -1 = nothing published
    std::atomic<int> activeSlot  { -1 };  // -1 = consumer holds nothing
    int              stagedIdx   { -1 };  // producer-thread only
};
