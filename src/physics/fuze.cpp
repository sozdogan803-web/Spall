#include "physics/fuze.h"

#include <cstddef>

namespace phys {

FuzeEvent fuzeEvent(const FuzeSpec& fuze, double impactVelocity, const PenetrationResult& result) {
    FuzeEvent e;
    const std::vector<PenetrationSample>& h = result.history;
    if (fuze.type == FuzeType::None || h.empty()) return e;

    const double threshold = impactVelocity - fuze.triggerDv;
    for (size_t i = 0; i < h.size(); ++i) {
        if (h[i].velocity > threshold) continue;
        e.triggered = true;
        e.triggerLayer = h[i].layer;
        e.triggerTime = h[i].time;
        if (i > 0 && h[i - 1].velocity > h[i].velocity) {
            const double f = (h[i - 1].velocity - threshold) / (h[i - 1].velocity - h[i].velocity);
            e.triggerTime = h[i - 1].time + f * (h[i].time - h[i - 1].time);
        }
        break;
    }
    if (!e.triggered) return e;

    e.burstTime = e.triggerTime + (fuze.type == FuzeType::BaseDelay ? fuze.delay : fuze.functionTime);
    const PenetrationSample& last = h.back();
    if (e.burstTime >= last.time) {
        if (result.perforated) {
            e.burstDepth = last.depth + result.residualVelocity * (e.burstTime - last.time);
        } else {
            e.burstDepth = last.depth;
            e.stopped = true;
        }
    } else {
        size_t i = 1;
        while (i < h.size() && h[i].time < e.burstTime) ++i;
        const PenetrationSample& a = h[i - 1];
        const PenetrationSample& b = h[i];
        const double f = b.time > a.time ? (e.burstTime - a.time) / (b.time - a.time) : 1.0;
        e.burstDepth = a.depth + f * (b.depth - a.depth);
    }
    for (size_t l = 0; l < result.layers.size(); ++l)
        if (e.burstDepth >= result.layers[l].front && e.burstDepth <= result.layers[l].back)
            e.burstLayer = static_cast<int>(l);
    return e;
}

} // namespace phys
