#ifndef SIM_WORLDSTATE_H_
#define SIM_WORLDSTATE_H_

#include <vector>
#include <span>

#include <Magnum/Math/Tags.h>
#include <Magnum/Math/Time.h>

#include "sim/CsgoMovement.h"
#include "sim/Entities/BumpmineProjectile.h"
#include "sim/Entities/Player.h"
#include "sim/PlayerInput.h"
#include "sim/Sim.h"

struct CsgoSubtickStep
{
    uint64_t inputBitmask;
    int tick;
    float when;
};


namespace sim {

class WorldState {
public:
    // Simulation time point of this world state.
    // Note: Other data in this world state refers to other simulation time
    //       points (e.g. next attack time point) that are related to this.
    //       -> Do not change this variable without considering this fact.
    SimTimePoint simtime{ Magnum::Math::ZeroInit }; // Time point 0 by default

    // Whether this worldstate was created by interpolating between two other
    // worldstates.
    // Note: An interpolated worldstate has a number of invalid properties since
    //       only a few properties from it get interpolated and used!
    bool is_interpolated = false;

    // Actual world state
    PlayerInput::State prev_input; // Last input this worldstate was advanced with
    CsgoMovement csgo_mv;
    Entities::Player player;
    std::vector<Entities::BumpmineProjectile> bumpmine_projectiles;


    // ----------------------------------------

    static WorldState Interpolate(const WorldState& stateA,
        const WorldState& stateB, float phase);

    // Advance this world state with the given chronological player input
    // forward in simulation time by the given duration.
    // CAUTION: Must not be called on an interpolated worldstate!
    void AdvanceSimulation(SimTimeDur simtime_delta,
                           std::span<const PlayerInput::State> chro_input, std::vector<CsgoSubtickStep> SubtickSteps = {}, float fraction = 1.0f);

};

} // namespace sim

#endif // SIM_WORLDSTATE_H_
