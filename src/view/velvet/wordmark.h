// The Optic Suite loading animation (port of Optic Suite's WordmarkView.kt, itself a port of
// optic-suite-loading-white.svg): the eye forms, glides left, OPTIC and "suite" slide out from
// behind it, the crosshair locks on the O, the beam scans, then it all reverses. 6 s per cycle.
#pragma once
#include <cstdint>

namespace wordmark {

constexpr float kCycle = 6.0f;  // seconds

// Draws frame [p] (0..1 of the cycle) as coverage (0..1, may exceed 1: clamp) into [cov]
// (w*h floats, cleared by the caller). Logo units map to pixels as x*s+ox, y*s+oy (the
// logo spans x -3..333, y 22..112).
void render(float p, float s, float ox, float oy, float* cov, int w, int h);

// Where "suite" is drawn this frame, in logo units: its left x, baseline y, the advance per
// letter, and the x left of which it is hidden (behind the eye).
struct Suite { float x, base, advance, clip_x; };
Suite suite(float p);

}  // namespace wordmark
