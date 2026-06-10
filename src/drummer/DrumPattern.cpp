#include "DrumPattern.h"
#include <algorithm>
#include <cstring>

DrumPattern::DrumPattern()
{
    loadStyle (Style::Rock);
}

uint8_t DrumPattern::getStep (int stepIndex) const
{
    if (stepIndex < 0 || stepIndex >= kSteps) return 0;
    return layers[static_cast<int> (currentDensity)][stepIndex];
}

int DrumPattern::getLengthInSamples (double bpm, double sampleRate) const
{
    if (bpm <= 0.0 || sampleRate <= 0.0) return 0;
    return static_cast<int> ((4.0 * 60.0 / bpm) * sampleRate);
}

void DrumPattern::loadStyle (Style style)
{
    currentStyle = style;
    switch (style)
    {
        case Style::Rock:        buildRock();        break;
        case Style::Jazz:        buildJazz();        break;
        case Style::Electronic:  buildElectronic();  break;
    }
}

void DrumPattern::setDensity (Density density)
{
    currentDensity = density;
}

// ── Pattern builders ──────────────────────────────────────────────────────────
// Bit values: Kick=0x01 Snare=0x02 HiHat=0x04 Crash=0x08 Ride=0x10 Tom=0x20

void DrumPattern::buildRock()
{
    // ── Sparse: half-time feel — kick 1&3, snare 2&4, half-time hats ─────────
    std::memset (layers[0], 0, kSteps);
    layers[0][ 0] = 0x05;  // Kick | HiHat
    layers[0][ 4] = 0x06;  // Snare | HiHat
    layers[0][ 8] = 0x05;  // Kick | HiHat
    layers[0][12] = 0x06;  // Snare | HiHat

    // ── Medium: standard rock beat — 8th-note hats ────────────────────────────
    std::memset (layers[1], 0, kSteps);
    layers[1][ 0] = 0x05;  // Kick | HiHat
    layers[1][ 2] = 0x04;  // HiHat
    layers[1][ 4] = 0x06;  // Snare | HiHat
    layers[1][ 6] = 0x04;  // HiHat
    layers[1][ 8] = 0x05;  // Kick | HiHat
    layers[1][10] = 0x04;  // HiHat
    layers[1][12] = 0x06;  // Snare | HiHat
    layers[1][14] = 0x04;  // HiHat

    // ── Full: 16th-note hats, crash on 1, kick variation, tom fill ────────────
    std::memset (layers[2], 0x04, kSteps);  // HiHat on every step
    layers[2][ 0] = 0x0D;  // Crash | Kick | HiHat
    layers[2][ 2] = 0x05;  // Kick | HiHat
    layers[2][ 4] = 0x06;  // Snare | HiHat
    layers[2][ 8] = 0x05;  // Kick | HiHat
    layers[2][10] = 0x05;  // Kick | HiHat
    layers[2][12] = 0x06;  // Snare | HiHat
    layers[2][13] = 0x24;  // Tom | HiHat
    layers[2][14] = 0x24;  // Tom | HiHat
}

void DrumPattern::buildJazz()
{
    // ── Sparse: ride on quarters, kick on 1, snare on 2&4 ────────────────────
    std::memset (layers[0], 0, kSteps);
    layers[0][ 0] = 0x11;  // Kick | Ride
    layers[0][ 4] = 0x12;  // Snare | Ride
    layers[0][ 8] = 0x10;  // Ride
    layers[0][12] = 0x12;  // Snare | Ride

    // ── Medium: ride on 8ths, kick on 1&3, snare on 2&4 ─────────────────────
    std::memset (layers[1], 0, kSteps);
    layers[1][ 0] = 0x11;  // Kick | Ride
    layers[1][ 2] = 0x10;  // Ride
    layers[1][ 4] = 0x12;  // Snare | Ride
    layers[1][ 6] = 0x10;  // Ride
    layers[1][ 8] = 0x11;  // Kick | Ride
    layers[1][10] = 0x10;  // Ride
    layers[1][12] = 0x12;  // Snare | Ride
    layers[1][14] = 0x10;  // Ride

    // ── Full: ride on all 16ths, syncopated kick, snare, tom fills ────────────
    std::memset (layers[2], 0x10, kSteps);  // Ride on every step
    layers[2][ 0] = 0x11;  // Kick | Ride
    layers[2][ 4] = 0x12;  // Snare | Ride
    layers[2][ 8] = 0x11;  // Kick | Ride
    layers[2][11] = 0x11;  // Kick | Ride  (syncopated)
    layers[2][12] = 0x12;  // Snare | Ride
    layers[2][13] = 0x30;  // Tom | Ride
    layers[2][14] = 0x30;  // Tom | Ride
}

void DrumPattern::buildElectronic()
{
    // ── Sparse: four-on-floor kick, snare on 2&4, no hats ────────────────────
    std::memset (layers[0], 0, kSteps);
    layers[0][ 0] = 0x01;  // Kick
    layers[0][ 4] = 0x03;  // Kick | Snare
    layers[0][ 8] = 0x01;  // Kick
    layers[0][12] = 0x03;  // Kick | Snare

    // ── Medium: four-on-floor + 8th-note hats ─────────────────────────────────
    std::memset (layers[1], 0, kSteps);
    layers[1][ 0] = 0x05;  // Kick | HiHat
    layers[1][ 2] = 0x04;  // HiHat
    layers[1][ 4] = 0x07;  // Kick | Snare | HiHat
    layers[1][ 6] = 0x04;  // HiHat
    layers[1][ 8] = 0x05;  // Kick | HiHat
    layers[1][10] = 0x04;  // HiHat
    layers[1][12] = 0x07;  // Kick | Snare | HiHat
    layers[1][14] = 0x04;  // HiHat

    // ── Full: four-on-floor + 16th-note hats + crash + off-beat accents ───────
    std::memset (layers[2], 0x04, kSteps);  // HiHat on every step
    layers[2][ 0] = 0x0D;  // Crash | Kick | HiHat
    layers[2][ 4] = 0x07;  // Kick | Snare | HiHat
    layers[2][ 8] = 0x05;  // Kick | HiHat
    layers[2][10] = 0x05;  // Kick | HiHat  (off-beat accent)
    layers[2][12] = 0x07;  // Kick | Snare | HiHat
}
