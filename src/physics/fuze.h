// When a fuzed round's fuze triggers and where its filler bursts, from the engineering model's
// velocity history. A fuze triggers only on velocity loss, never on contact alone.
#pragma once

#include "physics/penetration.h"

namespace phys {

struct FuzeEvent {
    bool triggered = false;
    double triggerTime = 0;   // s, same clock as the history
    int triggerLayer = -1;    // layer at the nose when it triggers (-1 in a gap)
    double burstTime = 0;     // s: trigger + delay (base delay) or + function time (nose contact)
    double burstDepth = 0;    // m along the shot line from the first face
    int burstLayer = -1;      // plate containing the burst point; -1 in a gap or behind the stack
    bool stopped = false;     // the round stopped before the burst: it bursts where it stopped
};

// Trigger: the first time v(t) <= impactVelocity - triggerDv (linear between samples). Burst depth:
// the nose depth at the burst time, extrapolated with the residual velocity past the last sample
// when the stack is perforated, else the last depth (stopped).
FuzeEvent fuzeEvent(const FuzeSpec& fuze, double impactVelocity, const PenetrationResult& result);

} // namespace phys
