#pragma once

#include "types.hpp"
#include <utility>
#include <vector>

namespace city {

struct CityProp {
    enum class Kind {
        StreetLight = 0,
        Tree,
        PlazaBench,
        PerimeterPylon,
        DataNode
    };
    Kind kind = Kind::StreetLight;
    float x = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    float scale = 1.0f;
    uint32_t seed = 0;
};

struct RoadSpec {
    static constexpr float kMainHalfRoad = 3.8f;
    static constexpr float kMainHalfCorridor = 5.4f;
    static constexpr float kSecHalfRoad = 2.3f;
    static constexpr float kSecHalfCorridor = 3.6f;
    static constexpr float kSecSpacing = 32.0f;
    static constexpr float kDistrictHalfExtent = 96.0f;
};

class CityLayout {
public:
    static std::vector<Building> BuildFromNodes(const std::vector<DirNode*>& nodes, float city_half_extent = 96.0f);
    static std::vector<Building> BuildFromWeightedNodes(const std::vector<std::pair<DirNode*, uint64_t>>& nodes, float city_half_extent = 96.0f);
    static std::vector<CityProp> GenerateProps(const std::vector<Building>& buildings);
};

} // namespace city
