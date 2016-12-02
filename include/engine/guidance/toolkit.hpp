#ifndef OSRM_ENGINE_GUIDANCE_TOOLKIT_HPP_
#define OSRM_ENGINE_GUIDANCE_TOOLKIT_HPP_

#include "extractor/guidance/turn_instruction.hpp"
#include "engine/guidance/route_step.hpp"
#include "util/bearing.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

#include <boost/range/iterator_range.hpp>

namespace osrm
{
namespace engine
{
namespace guidance
{

inline extractor::guidance::DirectionModifier::Enum angleToDirectionModifier(const double bearing)
{
    if (bearing < 135)
    {
        return extractor::guidance::DirectionModifier::Right;
    }

    if (bearing <= 225)
    {
        return extractor::guidance::DirectionModifier::Straight;
    }
    return extractor::guidance::DirectionModifier::Left;
}

// Runs fn on RouteStep sub-ranges determined to be roundabouts.
// The function fn is getting called with a roundabout range as in: [enter, .., leave].
//
// The following situations are taken care for (i.e. we discard them):
//  - partial roundabout: enter without exit or exit without enter
//  - data issues: no roundabout, exit before enter
template <typename Iter, typename Fn> inline Fn forEachRoundabout(Iter first, Iter last, Fn fn)
{
    while (first != last)
    {
        const auto enter = std::find_if(first, last, [](const RouteStep &step) {
            return entersRoundabout(step.maneuver.instruction);
        });

        // enter has to come before leave, otherwise: faulty data / partial roundabout, skip those
        const auto leave = std::find_if(enter, last, [](const RouteStep &step) {
            return leavesRoundabout(step.maneuver.instruction);
        });

        // No roundabouts, or partial one (like start / end inside a roundabout)
        if (enter == last || leave == last)
            break;

        (void)fn(std::make_pair(enter, leave));

        // Skip to first step after the currently handled enter / leave pair
        first = std::next(leave);
    }

    return fn;
}

LaneID inline numLanesToTheRight(const engine::guidance::RouteStep &step)
{
    return step.intersections.front().lanes.first_lane_from_the_right;
}

LaneID inline numLanesToTheLeft(const engine::guidance::RouteStep &step)
{
    LaneID const total = step.intersections.front().lane_description.size();
    return total - (step.intersections.front().lanes.lanes_in_turn +
                    step.intersections.front().lanes.first_lane_from_the_right);
}

auto inline lanesToTheLeft(const engine::guidance::RouteStep &step)
{
    const auto &description = step.intersections.front().lane_description;
    LaneID num_lanes_left = numLanesToTheLeft(step);
    return boost::make_iterator_range(description.begin(), description.begin() + num_lanes_left);
}

auto inline lanesToTheRight(const engine::guidance::RouteStep &step)
{
    const auto &description = step.intersections.front().lane_description;
    LaneID num_lanes_right = numLanesToTheRight(step);
    return boost::make_iterator_range(description.end() - num_lanes_right, description.end());
}

} // namespace guidance
} // namespace engine
} // namespace osrm

#endif /* OSRM_ENGINE_GUIDANCE_TOOLKIT_HPP_ */
