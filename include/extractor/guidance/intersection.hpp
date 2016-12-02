#ifndef OSRM_EXTRACTOR_GUIDANCE_INTERSECTION_HPP_
#define OSRM_EXTRACTOR_GUIDANCE_INTERSECTION_HPP_

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "util/angle_calculations.hpp"
#include "util/bearing.hpp"
#include "util/guidance/toolkit.hpp"
#include "util/node_based_graph.hpp"
#include "util/typedefs.hpp" // EdgeID

#include "extractor/guidance/turn_instruction.hpp"

#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/algorithm/find_if.hpp>

namespace osrm
{
namespace extractor
{
namespace guidance
{

// the shape of an intersection only knows about edge IDs and bearings
struct IntersectionShapeData
{
    EdgeID eid;
    double bearing;
    double segment_length;
};

inline auto makeCompareShapeDataByBearing(const double base_bearing)
{
    return [base_bearing](const IntersectionShapeData &lhs, const IntersectionShapeData &rhs) {
        return util::bearing::angleBetweenBearings(base_bearing, lhs.bearing) <
               util::bearing::angleBetweenBearings(base_bearing, rhs.bearing);
    };
};

// When viewing an intersection from an incoming edge, we can transform a shape into a view which
// gives additional information on angles and whether a turn is allowed
struct IntersectionViewData : IntersectionShapeData
{
    IntersectionViewData(const IntersectionShapeData &shape,
                         const bool entry_allowed,
                         const double angle)
        : IntersectionShapeData(shape), entry_allowed(entry_allowed), angle(angle)
    {
    }

    bool entry_allowed;
    double angle;

    bool CompareByAngle(const IntersectionViewData &other) const;
};

// A Connected Road is the internal representation of a potential turn. Internally, we require
// full list of all connected roads to determine the outcome.
// The reasoning behind is that even invalid turns can influence the perceived angles, or even
// instructions themselves. An pososible example can be described like this:
//
// aaa(2)aa
//          a - bbbbb
// aaa(1)aa
//
// will not be perceived as a turn from (1) -> b, and as a U-turn from (1) -> (2).
// In addition, they can influence whether a turn is obvious or not. b->(2) would also be no
// turn-operation,
// but rather a name change.
//
// If this were a normal intersection with
//
// cccccccc
//            o  bbbbb
// aaaaaaaa
//
// We would perceive a->c as a sharp turn, a->b as a slight turn, and b->c as a slight turn.
struct ConnectedRoad final : IntersectionViewData
{
    ConnectedRoad(const IntersectionViewData &view,
                  const TurnInstruction instruction,
                  const LaneDataID lane_data_id)
        : IntersectionViewData(view), instruction(instruction), lane_data_id(lane_data_id)
    {
    }

    TurnInstruction instruction;
    LaneDataID lane_data_id;

    // used to sort the set of connected roads (we require sorting throughout turn handling)
    bool compareByAngle(const ConnectedRoad &other) const;

    // make a left turn into an equivalent right turn and vice versa
    void mirror();

    OSRM_ATTR_WARN_UNUSED
    ConnectedRoad getMirroredCopy() const;
};

// small helper function to print the content of a connected road
std::string toString(const ConnectedRoad &road);

using IntersectionShape = std::vector<IntersectionShapeData>;

struct IntersectionView final : std::vector<IntersectionViewData>
{
    using Base = std::vector<IntersectionViewData>;

    bool valid() const
    {
        return std::is_sorted(begin(), end(), std::mem_fn(&IntersectionViewData::CompareByAngle));
    };

    Base::iterator findClosestTurn(double angle);
    Base::const_iterator findClosestTurn(double angle) const;
};

struct Intersection final : std::vector<ConnectedRoad>
{
    using Base = std::vector<ConnectedRoad>;
    /*
     * find the turn whose angle offers the least angularDeviation to the specified angle
     * E.g. for turn angles [0,90,260] and a query of 180 we return the 260 degree turn (difference
     * 80 over the difference of 90 to the 90 degree turn)
     */
    Base::iterator findClosestTurn(double angle);
    Base::const_iterator findClosestTurn(double angle) const;

    /*
     * Check validity of the intersection object. We assume a few basic properties every set of
     * connected roads should follow throughout guidance pre-processing. This utility function
     * allows checking intersections for validity
     */
    bool valid() const;
};

namespace intersection_helpers
{

// find the edge associated with a given eid
template <class IntersectionType>
typename IntersectionType::const_iterator findRoadForEid(const IntersectionType &intersection,
                                                         const EdgeID eid)
{
    return std::find_if(intersection.begin(), intersection.end(), [eid](const auto &road) {
        return road.eid == eid;
    });
}

template <class IntersectionType>
typename IntersectionType::const_iterator findClosestBearing(const IntersectionType &intersection,
                                                             const double bearing)
{
    return std::min_element(
        intersection.begin(), intersection.end(), [bearing](const auto &lhs, const auto &rhs) {
            return util::guidance::angularDeviation(lhs.bearing, bearing) <
                   util::guidance::angularDeviation(rhs.bearing, bearing);
        });
}

// the FilterType needs to be a function, returning false for elements to keep and true for elements
// to remove from the considerations.
template <class IntersectionType, class UnaryPredicate>
typename IntersectionType::const_iterator findClosestTurn(const IntersectionType &intersection,
                                                          const double angle,
                                                          const UnaryPredicate filter)
{
    const auto candidate = std::min_element(
        intersection.begin(),
        intersection.end(),
        [angle, &filter](const auto &lhs, const auto &rhs) {
            const auto filtered_lhs = filter(lhs), filtered_rhs = filter(rhs);
            const auto deviation_lhs = util::guidance::angularDeviation(lhs.angle, angle),
                       deviation_rhs = util::guidance::angularDeviation(rhs.angle, angle);
            return std::tie(filtered_lhs, deviation_lhs) < std::tie(filtered_rhs, deviation_rhs);
        });

    // make sure only to return valid elements
    return filter(*candidate) ? intersection.cend() : candidate;
}

// given all possible turns, which is the highest connected number of lanes per turn. This value
// is used, for example, during generation of intersections.
template <class IntersectionType>
std::uint8_t getHighestConnectedLaneCount(const IntersectionType &intersection,
                                          const util::NodeBasedDynamicGraph &node_based_graph)
{
    BOOST_ASSERT(!intersection.empty());
    using RoadDataType = typename IntersectionType::value_type;

    const std::function<std::uint8_t(const RoadDataType &)> to_lane_count =
        [&](const RoadDataType &road) {
            return node_based_graph.GetEdgeData(road.eid).road_classification.GetNumberOfLanes();
        };

    std::uint8_t max_lanes = 0;
    const auto extract_maximal_value = [&max_lanes](std::uint8_t value) {
        max_lanes = std::max(max_lanes, value);
        return false;
    };

    const auto view = intersection | boost::adaptors::transformed(to_lane_count);
    boost::range::find_if(view, extract_maximal_value);
    return max_lanes;
}
} // namespace intersection_helpers

Intersection::const_iterator findClosestTurn(const Intersection &intersection, const double angle);
Intersection::iterator findClosestTurn(Intersection &intersection, const double angle);

} // namespace guidance
} // namespace extractor
} // namespace osrm

#endif /*OSRM_EXTRACTOR_GUIDANCE_INTERSECTION_HPP_*/
