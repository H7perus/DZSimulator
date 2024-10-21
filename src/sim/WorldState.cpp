#include "sim/WorldState.h"

#include <algorithm>
#include <cassert>

#include <Tracy.hpp>

#include <Magnum/Magnum.h>
#include <Magnum/Math/Functions.h>
#include <Magnum/Math/Time.h>
#include <Magnum/Math/Vector3.h>

#include "GlobalVars.h"
#include "sim/CsgoConstants.h"
#include "sim/CsgoMovement.h"
#include "sim/PlayerInput.h"
#include "sim/Sim.h"
#include "utils_3d.h"

using namespace Magnum;

using namespace utils_3d;
using namespace coll;
using namespace sim;

WorldState WorldState::Interpolate(const WorldState& stateA,
                                   const WorldState& stateB,
                                   float phase)
{
    // We are assuming B comes after A, chronologically.
    assert(stateA.simtime <= stateB.simtime);

    if (phase <= 0.0f) return stateA;
    if (phase >= 1.0f) return stateB;

    // NOTE: Copying stateB is important in order for newly created entities
    //       (present in stateB, but not in stateA) to be propagated to future
    //       interpolated world states inside CsgoGame!
    WorldState interpState = stateB;

    interpState.is_interpolated = true;

    //interpState.simtime = (1.0f - phase) * stateA.simtime +  // H7per: This is lossy, likes to break with large values.
    //                      (       phase) * stateB.simtime;   // Sometimes the program crashes due to this, next interp will fail the assert.

    interpState.simtime = stateA.simtime + (stateB.simtime - stateA.simtime) * phase; // H7per: Better version, does multiplication with the smallest integer, keeping error lower.

    // NOTE: Player movement state is only partially being interpolated.
    interpState.csgo_mv.m_vecAbsOrigin  =
        (1.0f - phase) * stateA.csgo_mv.m_vecAbsOrigin +
        (       phase) * stateB.csgo_mv.m_vecAbsOrigin;
    interpState.csgo_mv.m_vecViewOffset =
        (1.0f - phase) * stateA.csgo_mv.m_vecViewOffset +
        (       phase) * stateB.csgo_mv.m_vecViewOffset;

    using BumpmineProjectile = Entities::BumpmineProjectile;
    for (BumpmineProjectile& bm_from_B : interpState.bumpmine_projectiles)
    {
        const auto& same_bm_from_A = std::find_if(
            stateA.bumpmine_projectiles.begin(),
            stateA.bumpmine_projectiles.end(),
            [&bm_from_B](const BumpmineProjectile& bm) {
                return bm.unique_id == bm_from_B.unique_id;
            }
        );
        if (same_bm_from_A == stateA.bumpmine_projectiles.end())
            continue;

        bm_from_B.position = (1.0f - phase) * same_bm_from_A->position +
                             (       phase) * bm_from_B.position;

        // TODO Interpolate rotation here once Bump Mines rotate in the air?
        // TODO Interpolate other Bump Mine properties?
    }

    return interpState;
}

void WorldState::AdvanceSimulation(SimTimeDur simtime_delta,
    std::span<const PlayerInput::State> chro_input, std::vector<CsgoSubtickStep> SubtickSteps, float fraction) //H7per: fraction is needed so you can do less than full timesteps for per-frame-prediction properly
{
    ZoneScoped;

    assert(!is_interpolated); // We shouldn't simulate interpolated world states

    assert(fraction <= 1.0f); //H7per: This shouldn't happen but lets be sure.
    if (SubtickSteps.size())
    {
        assert(SubtickSteps.back().when <= fraction);
    }
    // Advance this worldstate's simulation time point. This must happen early
    // to let the following simulation code know at what point in time we are.
    simtime += simtime_delta;

    float time_delta_sec = (float)Seconds{ simtime_delta };

    // Abort if no map is loaded
    if (!g_coll_world)
        return;

    // Determine what player input we're going to simulate with
    PlayerInput::State used_input;
    if (chro_input.empty()) {
        // If there is no player input, we assume that inputs remain unchanged
        // from the last simulation advancement.
        used_input = prev_input; // Note: Its real-time sample time is old by now
        // Scrollwheel jump inputs don't persist across simulation advancements
        used_input.scrollwheel_jumped = false;
    }
    else {
        // If there is player input, use the latest one and check whether the
        // user scrollwheel-jumped at any point.
        used_input = chro_input.back(); // Use latest input

        bool scrollwheel_jumped_at_any_point = false;
        for (const PlayerInput::State& entry : chro_input)
            if (entry.scrollwheel_jumped)
                scrollwheel_jumped_at_any_point = true;

        used_input.scrollwheel_jumped = scrollwheel_jumped_at_any_point;
    }

    // Apply viewing angle input
    csgo_mv.m_vecViewAngles = {
        used_input.viewing_angles[0], // Pitch
        used_input.viewing_angles[1], // Yaw
        0.0f
    };

    // Apply button input
    csgo_mv.m_nButtons = used_input.nButtons;

    // If the user scrollwheel jumped, set the jump input for _this_
    // advancement of player movement simulation.
    if (used_input.scrollwheel_jumped && csgo_mv.m_MoveType != MOVETYPE_NOCLIP)
        csgo_mv.m_nButtons |= IN_JUMP;


    // ---- SIMULATE CS:GO GAME ----

    // Delete detonated Bump Mine projectiles
    std::erase_if(bumpmine_projectiles,
        [](const Entities::BumpmineProjectile& bm) { return bm.has_detonated; });

    // Simulate Bump Mine projectiles
    for (Entities::BumpmineProjectile& bm : bumpmine_projectiles)
        bm.AdvanceSimulation(simtime_delta, *this);

    // Spawn Bump Mine projectiles on mouse click
    if (csgo_mv.m_nButtons & IN_ATTACK) {
        // If player is allowed to attack
        if (simtime >= player.next_primary_attack) {
            // When the next attack will be allowed again
            player.next_primary_attack = simtime +
                RoundToNearestSimTimeStep(CSGO_BUMP_THROW_INTERVAL_SECS,
                                          simtime_delta);

            // Create Bump Mine projectile
            Vector3 forward;
            AnglesToVectors(csgo_mv.m_vecViewAngles, &forward);
            Entities::BumpmineProjectile bm;
            bm.unique_id =
                Entities::BumpmineProjectile::GenerateNewUniqueID();
            bm.position =
                csgo_mv.m_vecAbsOrigin +
                csgo_mv.m_vecViewOffset +
                Vector3(0.0f, 0.0f, -CSGO_BUMP_THROW_SPAWN_OFFSET);
            bm.velocity = csgo_mv.m_vecVelocity + CSGO_BUMP_THROW_SPEED * forward;
            bumpmine_projectiles.push_back(bm);
        }
    }

    // Let movement class know about player's equipment

    float prev_when = 0.0;
    bool already_stepped = false;

    used_input = prev_input;


    for (int step = 0; step <= SubtickSteps.size(); step++)
    {
        float curr_stepinterval;
        if (SubtickSteps.size() > 1)
        {
            printf("debug");
        }

        if (step < SubtickSteps.size())
        {
            csgo_mv.m_nButtons = used_input.nButtons;
            curr_stepinterval = SubtickSteps[step].when - prev_when;
            prev_when = SubtickSteps[step].when;
            used_input.nButtons = SubtickSteps[step].inputBitmask;
        }
        else
        {
            curr_stepinterval = fraction - prev_when; // H7per: end sim where fraction ends
            csgo_mv.m_nButtons = used_input.nButtons;
            if(fraction == 1.0f) //H7per: doing this equal check is fine, since it will always be 1 for fullframe.
                prev_input = used_input;
        }

        

        csgo_mv.m_loadout = player.loadout;

        csgo_mv.m_flForwardMove = 0.0f;
        if (csgo_mv.m_nButtons & IN_FORWARD)
            csgo_mv.m_flForwardMove += g_csgo_game_sim_cfg.cl_forwardspeed;
        if (csgo_mv.m_nButtons & IN_BACK)
            csgo_mv.m_flForwardMove -= g_csgo_game_sim_cfg.cl_backspeed;

        csgo_mv.m_flSideMove = 0.0f;
        if (csgo_mv.m_nButtons & IN_MOVERIGHT)
            csgo_mv.m_flSideMove += g_csgo_game_sim_cfg.cl_sidespeed;
        if (csgo_mv.m_nButtons & IN_MOVELEFT)
            csgo_mv.m_flSideMove -= g_csgo_game_sim_cfg.cl_sidespeed;

        // -------- start of source-sdk-2013 code --------
        // (taken and modified from source-sdk-2013/<...>/src/game/shared/gamemovement.cpp)
        // (Original code found in ProcessMovement() function)

        // Cropping movement speed scales mv->m_fForwardSpeed etc. globally
        // Once we crop, we don't want to recursively crop again, so we set the crop
        //  flag globally here once per usercmd cycle.
        csgo_mv.m_iSpeedCropped = SPEED_CROPPED_RESET;

        // Init max speed depending on weapons equipped by player
        csgo_mv.m_flMaxSpeed =
            g_csgo_game_sim_cfg.GetMaxPlayerRunningSpeed(player.loadout);

        csgo_mv.PlayerMove(time_delta_sec * curr_stepinterval);
        csgo_mv.FinishMove();
    }
    
    // --------- end of source-sdk-2013 code ---------

    // For the next call of AdvanceSimulation(), remember what player inputs we
    // used in the current simulation advancement.
    //prev_input = used_input; //H7per: Lets try without these shenanigans, shall we?
}
