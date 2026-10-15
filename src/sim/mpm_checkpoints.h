// Checkpoints of a live deformation run, so the view can be scrubbed backwards without starting
// the simulation again: copies of the solver's state (MpmState) at even times through the run,
// as many as fit a share of the free video memory. A seek back resumes from the latest one at
// or before the time asked and steps on from there. The step is deterministic, so a checkpoint
// taken ahead of where the solver was sent back to is still the state the run will reach.
#pragma once

#include "sim/mpm_solver.h"

#include <limits>
#include <memory>
#include <vector>

class MpmCheckpoints {
public:
    static constexpr int kMaxSlots = 32;

    // Plans the checkpoints from the loaded scene's start to t1, as many as fit; none when one does
    // not fit (a seek back then starts again). Any taken before are dropped.
    void begin(const MpmSolver& solver, double t1);
    void clear();

    bool active() const { return !times_.empty(); }
    int planned() const { return static_cast<int>(times_.size()); }
    int taken() const { return static_cast<int>(states_.size()); }
    double end() const { return end_; }
    // When the next checkpoint still to be taken is due (infinity when all are): the solver should
    // stop there, then call capture.
    double nextTime() const;
    // Takes the checkpoint due if the solver has reached it (within half a step). reach is the
    // run's furthest projectile reach so far, kept with it.
    void capture(MpmSolver& solver, double reach);
    // Resumes the solver from the latest checkpoint at or before t (give or take half a step);
    // false when there is none (reset the solver instead). reach receives the reach kept with it.
    bool restore(MpmSolver& solver, double t, double& reach) const;

private:
    std::vector<double> times_;                       // planned, replaced by the solver's time as each is taken
    std::vector<std::unique_ptr<MpmState>> states_;  // a prefix of the plan
    std::vector<double> reach_;
    double end_ = 0;
};
