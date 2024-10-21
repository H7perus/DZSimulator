#include "CsgoGame.h"

#include <cassert>
#include <chrono>
#include <utility>

#include <Magnum/Math/Time.h>
#include <Magnum/Math/TimeStl.h>
#include <Tracy.hpp>

#include "common.h"
#include "sim/PlayerInput.h"
#include "sim/Sim.h"
#include "sim/WorldState.h"

using namespace sim;
using namespace Magnum;
using namespace Magnum::Math::Literals;

// Should be enabled, toggleable for debugging purposes
const bool ENABLE_INTERPOLATION_OF_DRAWN_WORLDSTATE = true;

CsgoGame::CsgoGame()
    : m_simtime_step_size{ 0.0_sec } // 0 indicates that game isn't started
    , m_realtime_game_tick_interval{}
    , m_realtime_game_start{}
    , m_prev_finalized_game_tick_id{ 0 }
    , m_prev_finalized_game_tick{}
    , m_inputs_since_prev_finalized_game_tick{}
    , m_prev_predicted_game_tick{}
    , m_prev_drawable_worldstate{}
    , m_prev_drawable_worldstate_timepoint{}
{
}

bool CsgoGame::HasBeenStarted() {
    return m_simtime_step_size > 0.0_sec;
}

void CsgoGame::UpdateTimescale(float simtime_scale)
{
    m_realtime_game_tick_interval = std::chrono::nanoseconds{
        Nanoseconds{ m_simtime_step_size / simtime_scale }
    };
}

void CsgoGame::Start(SimTimeDur simtime_step_size, float simtime_scale,
                     const WorldState& initial_worldstate)
{
    assert(simtime_step_size > 0.0_sec);
    assert(simtime_scale > 0.0f);

    WallClock::time_point current_realtime = WallClock::now();

    // NOTE: The simulation time point of the initial worldstate can be
    //       arbitrary!
    //       Real time and simulation time are distinct!

    m_simtime_step_size = simtime_step_size;
    m_realtime_game_tick_interval = std::chrono::nanoseconds{
        Nanoseconds{ simtime_step_size / simtime_scale }
    };
    m_realtime_game_start = current_realtime;
    m_realtime_last_tick = current_realtime;
    m_prev_finalized_game_tick_id = 0;
    m_prev_finalized_game_tick = initial_worldstate;
    m_inputs_since_prev_finalized_game_tick.clear();

    // Simulate one game tick to get a possible future game tick
    // @Optimization Instead of simulating a tick here, we should instead flag
    //               m_prev_predicted_game_tick as invalid and only simulate it
    //               on-demand inside ProcessNewPlayerInput().
    m_prev_predicted_game_tick = initial_worldstate;
    m_prev_predicted_game_tick.AdvanceSimulation(simtime_step_size, {});

    m_prev_drawable_worldstate = initial_worldstate;
    m_prev_drawable_worldstate_timepoint = current_realtime;
}

void CsgoGame::ModifyWorldStateHarshly(const std::function<void(WorldState&)>& f)
{
    ZoneScoped;

    if (!HasBeenStarted()) {
        assert(0);
        return;
    }

    // Run user-provided func that modifies this game's worldstate
    f(m_prev_finalized_game_tick);

    // Simulate one game tick to get a possible future game tick
    // @Optimization Instead of simulating a tick here, we should instead flag
    //               m_prev_predicted_game_tick as invalid and only simulate it
    //               on-demand inside ProcessNewPlayerInput().
    m_prev_predicted_game_tick = m_prev_finalized_game_tick;
    m_prev_predicted_game_tick.AdvanceSimulation(m_simtime_step_size, {});

    m_prev_drawable_worldstate = m_prev_finalized_game_tick;
    m_prev_drawable_worldstate_timepoint =
        GetGameTickRealTimePoint(m_prev_finalized_game_tick_id);
}

void CsgoGame::ProcessNewPlayerInput(const PlayerInput::State& new_input, bool subticked)
{
    ZoneScoped;

    if (!HasBeenStarted()) {
        assert(0);
        return;
    }

    // Note: We define that a player input affects a game tick if:
    //       (player_input_sample_timepoint <= game_tick_timepoint)

#ifndef NDEBUG
    // New input must have been sampled after all previously passed inputs.
    // New input is allowed to have been sampled at identical timepoints.
    for (const PlayerInput::State& other : m_inputs_since_prev_finalized_game_tick)
        assert(new_input.sample_time >= other.sample_time);
#endif
    
    WallClock::time_point cur_time = new_input.sample_time;


    // @Optimization We should drop game ticks if the user's machine struggles
    //               to keep up. How does the Source engine do it?

    // Step 1: Find ID of game tick that directly precedes the new player input.
    size_t directly_preceding_game_tick_id = m_prev_finalized_game_tick_id;
    //while (GetGameTickRealTimePoint(directly_preceding_game_tick_id + 1) < cur_time)
    //    directly_preceding_game_tick_id++;
    while (m_realtime_last_tick + m_realtime_game_tick_interval < cur_time)
    {
        directly_preceding_game_tick_id++;
        m_realtime_last_tick += m_realtime_game_tick_interval;
    }

    // Step 2: Advance game simulation up to and including directly preceding
    //         game tick, if not already done.

    // Preliminary action for this: If we need to advance by one or more new
    // game ticks, advance m_prev_finalized_game_tick already to the first next
    // game tick by simply copying the previously predicted game tick!
    // This is possible because it's certain that no new player inputs relevant
    // to that first tick advancement were generated.

    //H7per: Deprecating this, because it doesn't work with changing tick fractions.
    /*if (m_prev_finalized_game_tick_id < directly_preceding_game_tick_id) {

        m_prev_finalized_game_tick = std::move(m_prev_predicted_game_tick);
        m_prev_finalized_game_tick_id++;
        m_inputs_since_prev_finalized_game_tick.clear();
        prevSubtickSteps = SubtickSteps;
        SubtickSteps.clear();
    }*/


    // Next, possibly advance by additional # of game ticks.
    // These additional game ticks have passed completely without any calls to
    // ProcessNewPlayerInput(), so they receive no player input.
    while (m_prev_finalized_game_tick_id < directly_preceding_game_tick_id) {
        m_prev_finalized_game_tick.AdvanceSimulation(m_simtime_step_size, m_inputs_since_prev_finalized_game_tick, SubtickSteps);
        m_inputs_since_prev_finalized_game_tick.clear();
        prevSubtickSteps = SubtickSteps;
        SubtickSteps.clear();
        m_prev_finalized_game_tick_id++;
    }
    // NOTE: m_prev_predicted_game_tick has now become invalid if we advanced by
    //       one or more ticks.

    // Step 3: Predict the next future game tick using the new player input (and
    //         possibly previous inputs of the current unfinalized game tick).
    m_inputs_since_prev_finalized_game_tick.push_back(new_input);
    
    //if (new_input.scrollwheel_jumped) //H7per: Gotta allow scroll jump
    //{
    //    printf("");
    //}


    if ((SubtickSteps.empty() && m_prev_finalized_game_tick.prev_input.nButtons != new_input.nButtons) || !SubtickSteps.empty() && SubtickSteps.back().inputBitmask != new_input.nButtons || new_input.scrollwheel_jumped)
    {
        float curr_when = (float)(cur_time - m_realtime_last_tick).count() / (float)m_realtime_game_tick_interval.count();
        CsgoSubtickStep newStep;
        newStep.inputBitmask = new_input.nButtons;
        newStep.when = curr_when;
        if(!subticked)
            newStep.when = 0;
        newStep.tick = m_prev_finalized_game_tick_id;

        if (new_input.scrollwheel_jumped) //H7per: Gotta allow scroll jump
        {
            newStep.inputBitmask |= (2);
            if (subticked)
            {
                SubtickSteps.push_back(newStep);
                newStep.inputBitmask = newStep.inputBitmask & ~(2);
            }
                
        }

        SubtickSteps.push_back(newStep);

        if (SubtickSteps.size() > 1)
        {
            if (SubtickSteps.back().when < SubtickSteps.end()[-2].when)
            {
                printf("ALARM!");
            }
        }
    }
    
    if (SubtickSteps.size() > 0)
    {
        printf("");
    }

    WorldState predicted_next_game_tick = m_prev_finalized_game_tick;
    if(!subticked)
        predicted_next_game_tick.AdvanceSimulation( m_simtime_step_size,
                                                    m_inputs_since_prev_finalized_game_tick, SubtickSteps);

    WallClock::time_point next_game_tick_timepoint = m_realtime_last_tick + m_realtime_game_tick_interval;


    printf("current Tick-ID: ");
    printf( std::to_string(m_prev_finalized_game_tick_id + 1).c_str());
    printf( "\n");
    // Step 4: Determine current drawable world state by interpolating between
    //         previous drawable world state and the predicted next game tick.
    WorldState cur_drawable_worldstate;
    if (ENABLE_INTERPOLATION_OF_DRAWN_WORLDSTATE) {
        // @Optimization We could measure the current time again after the game
        //               tick simulations and use it for interpolation.
        //               This might help with responsiveness on low-end machines.
        //               CAUTION: This might lead to exceeding the interpolation
        //                        range! Handle that.
        WallClock::duration interpRange = next_game_tick_timepoint - m_prev_drawable_worldstate_timepoint;
        WallClock::duration interpStep  =                 cur_time - m_prev_drawable_worldstate_timepoint;

        if (subticked)
        {
            interpRange = next_game_tick_timepoint - m_realtime_last_tick;
            interpStep = cur_time - m_realtime_last_tick;
        }

        using nano = std::chrono::nanoseconds;
        float interpRange_ns = std::chrono::duration_cast<nano>(interpRange).count();
        float interpStep_ns  = std::chrono::duration_cast<nano>(interpStep ).count();
        float phase = interpStep_ns / interpRange_ns;


        if (phase > 0.45 && phase < 0.55)
        {
            printf("");
        }

        printf("%i, %i, %f", m_prev_predicted_game_tick.simtime, predicted_next_game_tick.simtime, phase, "\n");

        if (subticked)
        {

            cur_drawable_worldstate = m_prev_finalized_game_tick;
            cur_drawable_worldstate.AdvanceSimulation(m_simtime_step_size,
                m_inputs_since_prev_finalized_game_tick, SubtickSteps, phase);
        }
        else
        {
            if (interpRange_ns == 0.0f) {
                cur_drawable_worldstate = predicted_next_game_tick;
            }
            else {

                cur_drawable_worldstate = WorldState::Interpolate(m_prev_drawable_worldstate,
                    predicted_next_game_tick,
                    phase);
            }
        }


    }
    else { // ENABLE_INTERPOLATION_OF_DRAWN_WORLDSTATE == false
        // Instead of interpolating, just draw the last finalized game tick
        cur_drawable_worldstate = m_prev_finalized_game_tick;
    }

    // Remember for user access and future ProcessNewPlayerInput() calls
    m_prev_predicted_game_tick           = std::move(predicted_next_game_tick);
    if(subticked){
        m_prev_predicted_game_tick           = std::move(cur_drawable_worldstate);
    }
    m_prev_drawable_worldstate           = std::move(cur_drawable_worldstate);

    m_prev_drawable_worldstate_timepoint = cur_time;
}

const WorldState& CsgoGame::GetLatestActualWorldState() {
    assert(HasBeenStarted());
    return m_prev_finalized_game_tick;
}

const WorldState& CsgoGame::GetLatestDrawableWorldState() {
    assert(HasBeenStarted());
    return m_prev_drawable_worldstate;
}

WallClock::time_point CsgoGame::GetGameTickRealTimePoint(size_t tick_id) // H7per: This function doesn't work if you use it for the next tick time, if you want changable timescale.
{
    assert(HasBeenStarted());
    return m_realtime_game_start + tick_id * m_realtime_game_tick_interval;
}
