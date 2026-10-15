#include "sim/mpm_checkpoints.h"

#include "core/log.h"
#include "sim/mpm_frames.h"

#include <algorithm>
#include <cstddef>

void MpmCheckpoints::begin(const MpmSolver& solver, double t1) {
    clear();
    if (!solver.loaded()) return;
    end_ = t1;
    const int slots = mpmCheckpointSlots(solver.stateBytes(), gl::freeVideoBytes(), kMaxSlots);
    times_ = mpmCheckpointTimes(solver.scene().tStart, t1, slots);
}

void MpmCheckpoints::clear() {
    times_.clear();
    states_.clear();  // frees the video memory
    reach_.clear();
    end_ = 0;
}

double MpmCheckpoints::nextTime() const {
    return taken() < planned() ? times_[static_cast<size_t>(taken())] : std::numeric_limits<double>::infinity();
}

void MpmCheckpoints::capture(MpmSolver& solver, double reach) {
    const double half = 0.5 * solver.scene().dt;
    if (taken() >= planned() || solver.time() + half < times_[static_cast<size_t>(taken())]) return;
    auto state = std::make_unique<MpmState>();
    if (!solver.saveState(*state)) {
        // No memory for more: the ones taken stay, a seek further back starts again.
        logx::warn("scrub: out of GPU memory after %d checkpoints", taken());
        times_.resize(states_.size());
        return;
    }
    const size_t i = states_.size();
    // The solver's own time, which may differ from the plan by half a step; kept in order.
    times_[i] = std::max(solver.time(), i > 0 ? times_[i - 1] : solver.time());
    reach_.push_back(std::max(reach, solver.measure().reach));
    states_.push_back(std::move(state));
    // Planned times the solver is past (a jump forward) would only copy this same state.
    const auto first = times_.begin() + static_cast<std::ptrdiff_t>(i) + 1;
    times_.erase(first, std::find_if(first, times_.end(), [&](double t) { return t > solver.time() + half; }));
}

bool MpmCheckpoints::restore(MpmSolver& solver, double t, double& reach) const {
    const std::vector<double> got(times_.begin(), times_.begin() + taken());
    const int i = mpmPickCheckpoint(got, t, 0.5 * solver.scene().dt);
    if (i < 0 || !solver.restoreState(*states_[static_cast<size_t>(i)])) return false;
    reach = reach_[static_cast<size_t>(i)];
    return true;
}
