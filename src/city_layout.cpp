#include "city_layout.hpp"

#include <algorithm>
#include <cmath>

namespace city {

namespace {

uint32_t HashString(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

bool IntervalOverlaps(float a0, float a1, float b0, float b1) {
    return !(a1 <= b0 || a0 >= b1);
}

bool OverlapsRoadCorridor(float x0, float z0, float x1, float z1) {
    // Main central boulevards at X=0 and Z=0
    if (IntervalOverlaps(x0, x1, -RoadSpec::kMainHalfCorridor, RoadSpec::kMainHalfCorridor)) return true;
    if (IntervalOverlaps(z0, z1, -RoadSpec::kMainHalfCorridor, RoadSpec::kMainHalfCorridor)) return true;

    // Secondary avenues at +/- 32, 64, 96, 128
    for (int k = 1; k <= 4; ++k) {
        float c = static_cast<float>(k) * RoadSpec::kSecSpacing;
        if (IntervalOverlaps(x0, x1, c - RoadSpec::kSecHalfCorridor, c + RoadSpec::kSecHalfCorridor)) return true;
        if (IntervalOverlaps(x0, x1, -c - RoadSpec::kSecHalfCorridor, -c + RoadSpec::kSecHalfCorridor)) return true;
        if (IntervalOverlaps(z0, z1, c - RoadSpec::kSecHalfCorridor, c + RoadSpec::kSecHalfCorridor)) return true;
        if (IntervalOverlaps(z0, z1, -c - RoadSpec::kSecHalfCorridor, -c + RoadSpec::kSecHalfCorridor)) return true;
    }
    return false;
}

bool PointNearRoad(float x, float z, float margin) {
    if (std::abs(x) < RoadSpec::kMainHalfRoad + margin) return true;
    if (std::abs(z) < RoadSpec::kMainHalfRoad + margin) return true;
    for (int k = 1; k <= 4; ++k) {
        float c = static_cast<float>(k) * RoadSpec::kSecSpacing;
        if (std::abs(std::abs(x) - c) < RoadSpec::kSecHalfRoad + margin) return true;
        if (std::abs(std::abs(z) - c) < RoadSpec::kSecHalfRoad + margin) return true;
    }
    return false;
}

bool PointInsideAnyBuilding(float x, float z, const std::vector<Building>& buildings, float margin) {
    for (const auto& b : buildings) {
        if (x >= b.x0 - margin && x <= b.x1 + margin &&
            z >= b.z0 - margin && z <= b.z1 + margin) {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<Building> CityLayout::BuildFromNodes(const std::vector<DirNode*>& nodes, float city_half_extent) {
    std::vector<std::pair<DirNode*, uint64_t>> weighted;
    weighted.reserve(nodes.size());
    for (DirNode* n : nodes) {
        if (n && !n->deleted && n->total_bytes > 0) {
            weighted.push_back({n, n->total_bytes});
        }
    }
    return BuildFromWeightedNodes(weighted, city_half_extent);
}

std::vector<Building> CityLayout::BuildFromWeightedNodes(const std::vector<std::pair<DirNode*, uint64_t>>& nodes, float /*city_half_extent*/) {
    std::vector<Building> buildings;
    std::vector<std::pair<DirNode*, uint64_t>> valid;
    valid.reserve(nodes.size());
    for (const auto& [n, bytes] : nodes) {
        if (n && !n->deleted && bytes > 0) {
            valid.push_back({n, bytes});
        }
    }
    if (valid.empty()) return buildings;

    std::sort(valid.begin(), valid.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });

    constexpr size_t kMaxBuildings = 100;
    if (valid.size() > kMaxBuildings) {
        valid.resize(kMaxBuildings);
    }

    uint64_t max_bytes = valid.front().second;
    if (max_bytes == 0) max_bytes = 1;

    struct PlacedBox {
        float x0, z0, x1, z1;
    };
    std::vector<PlacedBox> placed;
    placed.reserve(valid.size());
    buildings.reserve(valid.size());

    constexpr float kBuildingGap = 2.4f;
    uint32_t next_id = 1;

    for (size_t i = 0; i < valid.size(); ++i) {
        DirNode* node = valid[i].first;
        uint64_t bytes = valid[i].second;
        double ratio = static_cast<double>(bytes) / static_cast<double>(max_bytes);

        // Footprint capped to fit cleanly inside 22.6-unit downtown blocks
        float base_side = 4.2f + 12.6f * static_cast<float>(std::pow(ratio, 0.28));
        float aspect = (i % 3 == 0) ? 1.12f : ((i % 3 == 1) ? 0.90f : 1.0f);
        float w = std::min(19.2f, base_side * aspect);
        float d = std::min(19.2f, base_side / aspect);
        float h = 3.2f + 46.0f * static_cast<float>(std::pow(ratio, 0.42));

        float best_cx = 16.8f;
        float best_cz = 16.8f;
        bool found_spot = false;

        float angle = static_cast<float>(i) * 2.39996f;
        float radius = (i == 0) ? 22.0f : 14.0f;
        for (int step = 0; step < 5500; ++step) {
            float cx = std::cos(angle) * radius;
            float cz = std::sin(angle) * radius;
            float x0 = cx - w * 0.5f;
            float z0 = cz - d * 0.5f;
            float x1 = cx + w * 0.5f;
            float z1 = cz + d * 0.5f;

            if (!OverlapsRoadCorridor(x0, z0, x1, z1)) {
                bool hit = false;
                for (const auto& pb : placed) {
                    if (!(x1 + kBuildingGap <= pb.x0 || x0 - kBuildingGap >= pb.x1 ||
                          z1 + kBuildingGap <= pb.z0 || z0 - kBuildingGap >= pb.z1)) {
                        hit = true;
                        break;
                    }
                }
                if (!hit) {
                    best_cx = cx;
                    best_cz = cz;
                    found_spot = true;
                    break;
                }
            }
            angle += 0.26f;
            radius += 0.20f;
        }

        if (!found_spot) {
            best_cx = 16.8f + static_cast<float>(i % 4) * 32.0f;
            best_cz = 16.8f + static_cast<float>(i / 4) * 32.0f;
        }

        float x0 = best_cx - w * 0.5f;
        float z0 = best_cz - d * 0.5f;
        float x1 = best_cx + w * 0.5f;
        float z1 = best_cz + d * 0.5f;
        placed.push_back({x0, z0, x1, z1});

        Building b{};
        b.node = node;
        b.label = node->name_utf8;
        b.size_label = FormatBytesUtf8(bytes);
        b.category = node->category;
        b.is_protected = node->is_protected;
        b.is_junk = node->is_junk;
        b.x0 = x0;
        b.z0 = z0;
        b.x1 = x1;
        b.z1 = z1;
        b.height = 0.15f;
        b.target_height = h;
        b.spawn_delay = std::min(0.85f, static_cast<float>(i) * 0.014f);
        b.spawn_timer = 0.0f;
        b.spawn_progress = 0.0f;
        b.style_seed = HashString(node->name_utf8);
        b.id = next_id++;
        b.bytes = bytes;
        buildings.push_back(std::move(b));
    }

    return buildings;
}

std::vector<CityProp> CityLayout::GenerateProps(const std::vector<Building>& buildings) {
    std::vector<CityProp> props;
    props.reserve(420);

    auto near_intersection = [](float v) {
        if (std::abs(v) < 6.5f) return true;
        for (int k = 1; k <= 3; ++k) {
            if (std::abs(std::abs(v) - static_cast<float>(k) * RoadSpec::kSecSpacing) < 5.2f) return true;
        }
        return false;
    };

    // 1. Streetlights along main central boulevards (X=0, Z=0)
    for (float t = -88.0f; t <= 88.0f; t += 11.0f) {
        if (near_intersection(t)) continue;
        // Along Z-axis boulevard (X = +/- 4.35)
        props.push_back({CityProp::Kind::StreetLight, -4.35f, t, 0.0f, 1.0f, 1});
        props.push_back({CityProp::Kind::StreetLight,  4.35f, t, 3.14159f, 1.0f, 2});
        // Along X-axis boulevard (Z = +/- 4.35)
        props.push_back({CityProp::Kind::StreetLight, t, -4.35f, 1.5708f, 1.0f, 3});
        props.push_back({CityProp::Kind::StreetLight, t,  4.35f, -1.5708f, 1.0f, 4});
    }

    // 2. Streetlights along secondary avenues (+/- 32, +/- 64)
    for (float road : {-64.0f, -32.0f, 32.0f, 64.0f}) {
        for (float t = -84.0f; t <= 84.0f; t += 16.0f) {
            if (near_intersection(t)) continue;
            props.push_back({CityProp::Kind::StreetLight, road - 2.85f, t, 0.0f, 0.88f, 5});
            props.push_back({CityProp::Kind::StreetLight, road + 2.85f, t, 3.14159f, 0.88f, 6});
            props.push_back({CityProp::Kind::StreetLight, t, road - 2.85f, 1.5708f, 0.88f, 7});
            props.push_back({CityProp::Kind::StreetLight, t, road + 2.85f, -1.5708f, 0.88f, 8});
        }
    }

    // 3. Central intersection corner data pylons
    for (float sx : {-5.1f, 5.1f}) {
        for (float sz : {-5.1f, 5.1f}) {
            props.push_back({CityProp::Kind::DataNode, sx, sz, 0.0f, 1.0f, 10});
        }
    }

    // 4. Urban trees along sidewalks & open block pockets
    uint32_t tree_seed = 100;
    for (float gx = -82.0f; gx <= 82.0f; gx += 9.5f) {
        for (float gz = -82.0f; gz <= 82.0f; gz += 9.5f) {
            ++tree_seed;
            if ((tree_seed % 3) != 0) continue;
            float jx = gx + static_cast<float>((tree_seed * 37) % 5 - 2) * 0.6f;
            float jz = gz + static_cast<float>((tree_seed * 53) % 5 - 2) * 0.6f;
            if (PointNearRoad(jx, jz, 1.3f)) continue;
            if (PointInsideAnyBuilding(jx, jz, buildings, 1.6f)) continue;
            float sc = 0.85f + static_cast<float>(tree_seed % 5) * 0.12f;
            props.push_back({CityProp::Kind::Tree, jx, jz, static_cast<float>(tree_seed % 6), sc, tree_seed});
        }
    }

    // 5. District perimeter pylons around +/- 96
    for (float t = -96.0f; t <= 96.0f; t += 24.0f) {
        if (std::abs(t) < 6.0f) continue;
        props.push_back({CityProp::Kind::PerimeterPylon, t, -96.0f, 0.0f, 1.0f, 20});
        props.push_back({CityProp::Kind::PerimeterPylon, t,  96.0f, 0.0f, 1.0f, 21});
        props.push_back({CityProp::Kind::PerimeterPylon, -96.0f, t, 1.5708f, 1.0f, 22});
        props.push_back({CityProp::Kind::PerimeterPylon,  96.0f, t, 1.5708f, 1.0f, 23});
    }

    return props;
}

} // namespace city
