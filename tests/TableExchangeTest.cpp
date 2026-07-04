#include <JuceHeader.h>
#include "drummer/TableExchange.h"
#include "drummer/DrumPattern.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace
{
    /** Deterministically fills every cell of a GrooveTable as a pure function
        of (seq, voice, step), plus styleSalt = seq (the embedded marker the
        consumer reads back to know which generation it is checking). Used by
        both the single-threaded state-machine tests (a "recognizable
        pattern") and the C1-style concurrency stress test (a torn-write
        detector: any cell that doesn't match this function for its own
        table's seq means the consumer observed a half-written slot). */
    void fillDeterministic (GrooveTable& t, uint32_t seq) noexcept
    {
        t.styleSalt = seq;
        t.swing     = static_cast<uint8_t> (seq & 0xFF);

        for (int v = 0; v < GrooveTable::kNumVoices; ++v)
        {
            for (int s = 0; s < GrooveTable::kNumSteps; ++s)
            {
                const uint32_t mix = seq * 2654435761u
                                    + static_cast<uint32_t> (v) * 97u
                                    + static_cast<uint32_t> (s) * 13u;
                StepCell& c = t.cells[v][s];
                c.velLow              = static_cast<uint8_t> (mix & 0x7Fu);
                c.velHigh             = static_cast<uint8_t> ((mix >> 7) & 0x7Fu);
                c.complexityThreshold = static_cast<uint8_t> ((mix >> 14) & 0xFFu);
                c.probability         = static_cast<uint8_t> ((mix >> 22) & 0xFFu);
                c.velRandRange        = static_cast<int8_t>  ((mix >> 3) & 0xFFu);
                c.flags               = static_cast<uint8_t> (mix & 0x07u);
            }
        }
    }

    /** Inverse of fillDeterministic: true iff every field of every cell (and
        swing) matches what fillDeterministic would have written for the
        table's own embedded seq (t.styleSalt). On mismatch, describes exactly
        which field/cell disagreed (for a clear stress-test failure message). */
    bool verifyDeterministic (const GrooveTable& t, juce::String& failReason)
    {
        const uint32_t seq = t.styleSalt;

        if (t.swing != static_cast<uint8_t> (seq & 0xFF))
        {
            failReason = "swing byte disagreed with seq " + juce::String (seq);
            return false;
        }

        for (int v = 0; v < GrooveTable::kNumVoices; ++v)
        {
            for (int s = 0; s < GrooveTable::kNumSteps; ++s)
            {
                const uint32_t mix = seq * 2654435761u
                                    + static_cast<uint32_t> (v) * 97u
                                    + static_cast<uint32_t> (s) * 13u;
                const StepCell& c = t.cells[v][s];
                const bool ok =
                       c.velLow              == static_cast<uint8_t> (mix & 0x7Fu)
                    && c.velHigh             == static_cast<uint8_t> ((mix >> 7) & 0x7Fu)
                    && c.complexityThreshold == static_cast<uint8_t> ((mix >> 14) & 0xFFu)
                    && c.probability         == static_cast<uint8_t> ((mix >> 22) & 0xFFu)
                    && c.velRandRange        == static_cast<int8_t>  ((mix >> 3) & 0xFFu)
                    && c.flags               == static_cast<uint8_t> (mix & 0x07u);

                if (! ok)
                {
                    failReason = "cell (voice " + juce::String (v) + ", step " + juce::String (s)
                               + ") disagreed with seq " + juce::String (seq);
                    return false;
                }
            }
        }
        return true;
    }
}

/**
 * Tests for TableExchange (roadmap Step 19 / PR 19a) — the dormant SPSC
 * 2-slot mailbox that will hand a dynamically-built GrooveTable from a
 * non-audio producer to the audio thread. This file exercises the protocol
 * directly (single-threaded state-machine checks matching the exact
 * ownership transitions documented in TableExchange.h) and then with a real
 * producer/consumer thread pair (matching the DrumSamplerTest.cpp "C1"
 * concurrency-stress idiom) to catch anything the single-threaded checks
 * can't: torn writes, staleness, or reordering under real contention.
 */
class TableExchangeTest : public juce::UnitTest
{
public:
    TableExchangeTest() : juce::UnitTest ("TableExchange", "drummer") {}

    void runTest() override
    {
        beginTest ("empty mailbox: consume() is nullptr, tryStage() succeeds");
        {
            TableExchange ex;
            expect (ex.consume() == nullptr, "nothing has been published yet");
            expect (ex.tryStage() != nullptr, "a fresh mailbox must offer a free slot");
        }

        beginTest ("publish -> consume round-trip, and a second consume() sees nothing new");
        {
            TableExchange ex;

            GrooveTable* slot = ex.tryStage();
            expect (slot != nullptr, "tryStage must succeed on a fresh mailbox");
            fillDeterministic (*slot, 777);
            ex.publish();

            const GrooveTable* got = ex.consume();
            expect (got != nullptr, "a published table must be consumable");
            expect (got == slot, "consume() must hand back the exact staged slot");
            juce::String reason;
            expect (verifyDeterministic (*got, reason), "round-tripped table must be byte-identical: " + reason);

            expect (ex.consume() == nullptr, "a second consume() with no new publish must return nullptr");
        }

        beginTest ("last-writer-wins before the first consume()");
        {
            TableExchange ex;

            GrooveTable* a = ex.tryStage();
            expect (a != nullptr, "tryStage must succeed for the first slot");
            fillDeterministic (*a, 100);
            ex.publish();

            // Nothing has been consumed yet, so slot A is still only pending
            // (not active) — tryStage() must hand out the OTHER slot.
            GrooveTable* b = ex.tryStage();
            expect (b != nullptr, "tryStage must succeed for the second slot");
            expect (b != a, "the second stage must land in the other slot while A is still pending");
            fillDeterministic (*b, 200);
            ex.publish();   // supersedes A's unconsumed publish

            const GrooveTable* got = ex.consume();
            expect (got != nullptr, "the second publish must be consumable");
            expect (got == b, "consume() must return B's contents, never A's superseded publish");
            expectEquals ((int) got->styleSalt, 200, "consumed table must be B's generation, not A's");
        }

        beginTest ("both slots held: tryStage() returns nullptr, then recovers after a further consume()");
        {
            TableExchange ex;

            GrooveTable* a = ex.tryStage();
            fillDeterministic (*a, 1);
            ex.publish();
            const GrooveTable* activeA = ex.consume();   // A is now active
            expect (activeA == a, "A must be adopted as active");

            GrooveTable* b = ex.tryStage();
            expect (b != nullptr && b != a, "the other slot must still be stageable while only one is active");
            fillDeterministic (*b, 2);
            ex.publish();   // B is now pending, A is still active: both slots spoken for

            expect (ex.tryStage() == nullptr,
                    "producer must see no free slot while one table is active and another pending");

            const GrooveTable* activeB = ex.consume();   // adopts B, frees A's old pendingSlot claim
            expect (activeB == b, "B must be adopted as the new active table");

            expect (ex.tryStage() != nullptr, "a slot must free up again after the further consume()");
        }

        beginTest ("releaseActive() frees the slot: both slots become stageable again");
        {
            TableExchange ex;

            GrooveTable* p1 = ex.tryStage();
            expect (p1 != nullptr, "tryStage must succeed on a fresh mailbox");
            fillDeterministic (*p1, 10);
            ex.publish();
            const GrooveTable* c0 = ex.consume();
            expect (c0 == p1, "must adopt the just-published slot");
            ex.releaseActive();

            // With both atomics back to -1, the mailbox is indistinguishable
            // from fresh: tryStage() must return a slot again immediately.
            GrooveTable* p2 = ex.tryStage();
            expect (p2 != nullptr, "the released slot must be stageable again");
            fillDeterministic (*p2, 20);
            ex.publish();

            // While p2's table is pending (not yet consumed), tryStage() must
            // land in the OTHER slot — proving the mailbox didn't get stuck
            // always handing out the same one after a release.
            GrooveTable* p3 = ex.tryStage();
            expect (p3 != nullptr && p3 != p2,
                    "the previously-active slot's sibling must also be independently stageable");
            fillDeterministic (*p3, 30);
            ex.publish();

            const GrooveTable* c1 = ex.consume();
            expect (c1 == p3, "last-writer-wins: consume() must return the most recent publish");
            ex.releaseActive();

            // Full circle: the very first slot (p1's) must be obtainable once more.
            GrooveTable* p4 = ex.tryStage();
            expect (p4 == p1, "after releasing again, the original slot must be stageable once more");
        }

        beginTest ("C1 — one producer + one consumer thread, ~1s stress: no torn writes, no staleness");
        {
            TableExchange ex;

            std::atomic<bool>     stop         { false };
            std::atomic<bool>     producerOk   { true };
            std::atomic<bool>     consumerOk   { true };
            std::atomic<uint32_t> producedSeq  { 0 };
            std::atomic<uint32_t> consumedSeq  { 0 };
            juce::String          failReason;   // single-writer (consumer thread) only; read after join()

            // Producer: message-thread role. tryStage() returning nullptr is
            // the expected, non-error "both slots held" state — coalesce and
            // retry (a brief yield), never spin-fail.
            std::thread producer ([&]
            {
                uint32_t seq = 1;
                while (! stop.load (std::memory_order_relaxed))
                {
                    GrooveTable* slot = ex.tryStage();
                    if (slot == nullptr)
                    {
                        std::this_thread::yield();
                        continue;
                    }
                    fillDeterministic (*slot, seq);
                    ex.publish();
                    producedSeq.store (seq, std::memory_order_relaxed);
                    ++seq;
                }
            });

            // Consumer: audio-thread role, simulating bar wraps on a short
            // fixed interval. Verifies every adopted table is fully written
            // (no torn write) and that observed seqs strictly increase (no
            // staleness/reordering).
            std::thread consumer ([&]
            {
                const auto tEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds (1000);
                uint32_t prevSeq = 0;

                while (std::chrono::steady_clock::now() < tEnd)
                {
                    if (const GrooveTable* t = ex.consume())
                    {
                        juce::String reason;
                        if (! verifyDeterministic (*t, reason))
                        {
                            consumerOk.store (false, std::memory_order_relaxed);
                            failReason = "consumer observed a partially-written table (seq "
                                       + juce::String (t->styleSalt) + "): " + reason;
                        }
                        else if (t->styleSalt <= prevSeq)
                        {
                            consumerOk.store (false, std::memory_order_relaxed);
                            failReason = "observed seq " + juce::String (t->styleSalt)
                                       + " did not strictly increase over previous seq "
                                       + juce::String (prevSeq);
                        }

                        prevSeq = t->styleSalt;
                        consumedSeq.store (prevSeq, std::memory_order_relaxed);
                    }

                    std::this_thread::sleep_for (std::chrono::milliseconds (3));   // simulated bar wrap
                }

                stop.store (true, std::memory_order_relaxed);
            });

            producer.join();
            consumer.join();

            expect (producerOk.load(),  "every tryStage()/publish() cycle must succeed");
            expect (consumerOk.load(),  failReason.isEmpty() ? juce::String ("consumer detected a protocol violation")
                                                              : failReason);

            const uint32_t finalProduced = producedSeq.load();
            const uint32_t finalConsumed = consumedSeq.load();
            expectGreaterThan ((int) finalProduced, 1, "the producer must have published many generations in ~1s");
            expect (finalProduced >= finalConsumed, "the consumer can never observe a seq the producer hasn't produced");
            expect (finalProduced - finalConsumed <= 1,
                    "the last consumed seq must be at most one publish behind the final produced seq (final="
                    + juce::String (finalProduced) + ", consumed=" + juce::String (finalConsumed) + ")");
        }
    }
};

static TableExchangeTest tableExchangeTest;
