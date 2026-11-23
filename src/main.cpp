#include "city_layout.hpp"
#include "city_renderer.hpp"
#include "scanner.hpp"
#include "types.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

enum class ViewMode {
    Directory = 0,
    TopJunk = 1
};

std::wstring PickFolderDialog(HWND owner) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg))) && dlg) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Выберите папку или диск для 3D-города");
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return result;
}

std::wstring GetUserProfileDir() {
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) return buf;
    return L"C:\\";
}

const char* CategoryNameRu(city::NodeCategory cat, bool is_protected) {
    if (is_protected) return "Система (защищено)";
    switch (cat) {
        case city::NodeCategory::Junk:   return "Мусор / Кэш";
        case city::NodeCategory::Media:  return "Игры / Медиа / Архивы";
        case city::NodeCategory::Dev:    return "Разработка / SDK";
        case city::NodeCategory::System: return "Система";
        case city::NodeCategory::Normal: return "Обычная папка / файл";
    }
    return "Другое";
}

ImU32 CategoryBadgeColor(city::NodeCategory cat, bool is_protected) {
    if (is_protected) return IM_COL32(80, 140, 225, 240);
    switch (cat) {
        case city::NodeCategory::Junk:   return IM_COL32(250, 92, 54, 245);
        case city::NodeCategory::Media:  return IM_COL32(190, 95, 245, 240);
        case city::NodeCategory::Dev:    return IM_COL32(42, 215, 195, 240);
        case city::NodeCategory::System: return IM_COL32(80, 140, 225, 240);
        case city::NodeCategory::Normal: return IM_COL32(62, 165, 245, 240);
    }
    return IM_COL32(62, 165, 245, 240);
}

ImVec4 CategoryVec4(city::NodeCategory cat, bool is_protected) {
    if (is_protected) return ImVec4(0.35f, 0.58f, 0.92f, 1.0f);
    switch (cat) {
        case city::NodeCategory::Junk:   return ImVec4(0.98f, 0.36f, 0.22f, 1.0f);
        case city::NodeCategory::Media:  return ImVec4(0.76f, 0.38f, 0.96f, 1.0f);
        case city::NodeCategory::Dev:    return ImVec4(0.16f, 0.85f, 0.76f, 1.0f);
        case city::NodeCategory::System: return ImVec4(0.35f, 0.58f, 0.92f, 1.0f);
        case city::NodeCategory::Normal: return ImVec4(0.24f, 0.65f, 0.96f, 1.0f);
    }
    return ImVec4(0.24f, 0.65f, 0.96f, 1.0f);
}

std::string ToLowerAscii(std::string s) {
    for (char& ch : s) {
        if (static_cast<unsigned char>(ch) < 128) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
    }
    return s;
}

float EaseOutBack(float t) {
    const float c1 = 1.25f;
    const float c3 = c1 + 1.0f;
    float x = t - 1.0f;
    return 1.0f + c3 * x * x * x + c1 * x * x;
}

struct UiState {
    ImFont* font_regular = nullptr;
    ImFont* font_bold = nullptr;
    ImFont* font_title = nullptr;

    double displayed_freed_bytes = 0.0;
    float freed_flash = 0.0f;
    float mode_pill_t = 0.0f;
    float inspector_reveal = 0.0f;
    float toast_anim = 0.0f;

    std::array<float, 5> cat_fracs{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    char search_buf[128]{};
    int filter_cat = -1; // -1 = All, else NodeCategory index

    bool orbit_mode = false;
    float orbit_angle = 0.48f;
    bool auto_cam_on_strike = true;
};

struct AppState {
    HWND hwnd = nullptr;
    city::CityRenderer renderer;
    city::DiskScanner scanner;

    std::vector<city::DriveInfo> drives;
    std::wstring active_scan_path;
    std::shared_ptr<city::DirNode> root_tree;
    city::DirNode* current_dir = nullptr;
    ViewMode view_mode = ViewMode::Directory;

    std::vector<city::Building> buildings;
    std::vector<city::CityProp> props;
    std::vector<city::PlaneStrike> strikes;
    std::vector<city::TrafficCar> cars;
    std::vector<city::Particle3D> particles;
    uint64_t static_mesh_epoch = 1;

    int hovered_idx = -1;
    int selected_idx = -1;

    city::Camera cam;
    bool rmb_looking = false;
    POINT last_mouse{};

    uint64_t session_freed_bytes = 0;
    int session_deleted_count = 0;

    bool confirm_delete_open = false;
    city::DirNode* pending_delete_node = nullptr;
    int pending_delete_building_idx = -1;

    std::string toast_msg;
    float toast_timer = 0.0f;
    float toast_max_timer = 4.5f;
    bool toast_is_error = false;

    UiState ui;
    std::mt19937 rng{std::random_device{}()};

    void InitTraffic() {
        cars.clear();
        const float road_centers[] = {-64.0f, -32.0f, 0.0f, 32.0f, 64.0f};
        const DirectX::XMFLOAT4 palette[] = {
            {0.85f, 0.88f, 0.94f, 1.0f},
            {0.18f, 0.56f, 0.88f, 1.0f},
            {0.82f, 0.28f, 0.24f, 1.0f},
            {0.24f, 0.78f, 0.68f, 1.0f},
            {0.92f, 0.72f, 0.22f, 1.0f},
        };
        std::uniform_real_distribution<float> pos_dist(-108.0f, 108.0f);
        std::uniform_real_distribution<float> spd_dist(11.0f, 22.0f);

        for (int i = 0; i < 42; ++i) {
            city::TrafficCar car{};
            bool along_z = (i % 2 == 0);
            int r_idx = i % 5;
            float rc = road_centers[r_idx];
            int dir = ((i / 5) % 2 == 0) ? 1 : -1;
            float lane_off = (rc == 0.0f) ? 1.85f : 1.10f;
            car.along_z = along_z;
            car.dir = dir;
            car.lane_coord = rc + (along_z ? static_cast<float>(dir) * lane_off : -static_cast<float>(dir) * lane_off);
            car.axis_pos = pos_dist(rng);
            car.speed = spd_dist(rng);
            car.color = palette[i % 5];
            cars.push_back(car);
        }
    }

    void ResetCamera() {
        ui.orbit_mode = false;
        cam.flying_to_target = true;
        cam.target_pos = {-54.0f, 68.0f, -98.0f};
        cam.target_yaw = 0.48f;
        cam.target_pitch = 0.48f;
    }

    void FocusOnBuilding(const city::Building& b) {
        ui.orbit_mode = false;
        float cx = (b.x0 + b.x1) * 0.5f;
        float cz = (b.z0 + b.z1) * 0.5f;
        float span = std::max(b.x1 - b.x0, b.z1 - b.z0);
        float dist = std::clamp(span * 1.9f + b.target_height * 1.1f, 20.0f, 70.0f);

        cam.flying_to_target = true;
        cam.target_pos = {cx, std::max(14.0f, b.target_height * 0.85f + dist * 0.48f), cz - dist};
        cam.target_yaw = 0.0f;
        cam.target_pitch = 0.44f;
    }

    void FrameStrikeView(const city::Building& b, const DirectX::XMFLOAT3& approach_dir) {
        if (!ui.auto_cam_on_strike) return;
        ui.orbit_mode = false;
        float cx = (b.x0 + b.x1) * 0.5f;
        float cz = (b.z0 + b.z1) * 0.5f;
        float h = std::max(6.0f, b.target_height);

        // Position camera off to the side of the flight path so the incoming plane + tower are both framed!
        DirectX::XMFLOAT3 side{-approach_dir.z, 0.0f, approach_dir.x};
        float dist = std::clamp(34.0f + h * 1.05f, 36.0f, 88.0f);
        DirectX::XMFLOAT3 eye{
            cx + side.x * dist * 0.82f - approach_dir.x * dist * 0.42f,
            std::clamp(h * 0.72f + 16.0f, 18.0f, 88.0f),
            cz + side.z * dist * 0.82f - approach_dir.z * dist * 0.42f
        };

        float dx = cx - eye.x;
        float dy = (h * 0.62f) - eye.y;
        float dz = cz - eye.z;
        float horiz = std::sqrt(dx * dx + dz * dz);

        cam.flying_to_target = true;
        cam.target_pos = eye;
        cam.target_yaw = std::atan2(dx, dz);
        cam.target_pitch = std::clamp(-std::atan2(dy, std::max(1.0f, horiz)), -0.9f, 1.25f);
    }

    void RebuildCity() {
        hovered_idx = -1;
        selected_idx = -1;
        buildings.clear();
        strikes.clear();
        if (!root_tree) return;

        std::vector<city::DirNode*> nodes;
        if (view_mode == ViewMode::TopJunk) {
            nodes = city::DiskScanner::CollectTopJunk(root_tree.get(), 100);
        } else {
            city::DirNode* dir = current_dir ? current_dir : root_tree.get();
            for (auto& ch : dir->children) {
                if (!ch->deleted && ch->total_bytes > 0) {
                    nodes.push_back(ch.get());
                }
            }
        }
        buildings = city::CityLayout::BuildFromNodes(nodes, 96.0f);
        props = city::CityLayout::GenerateProps(buildings);
        static_mesh_epoch++;
    }

    void EnterDirectory(city::DirNode* dir) {
        if (!root_tree || !dir || !dir->is_dir || dir->deleted) return;
        view_mode = ViewMode::Directory;
        current_dir = dir;
        RebuildCity();
        ResetCamera();
    }

    void GoUp() {
        if (view_mode == ViewMode::TopJunk) {
            view_mode = ViewMode::Directory;
            RebuildCity();
            ResetCamera();
            return;
        }
        if (current_dir && current_dir->parent) {
            current_dir = current_dir->parent;
            RebuildCity();
            ResetCamera();
        }
    }

    void StartScanPath(const std::wstring& path) {
        active_scan_path = path;
        root_tree.reset();
        current_dir = nullptr;
        buildings.clear();
        props.clear();
        strikes.clear();
        particles.clear();
        hovered_idx = -1;
        selected_idx = -1;
        static_mesh_epoch++;
        scanner.StartScan(path);
    }

    int FindBuildingIndex(city::DirNode* node, int hint_idx) const {
        if (hint_idx >= 0 && hint_idx < static_cast<int>(buildings.size()) &&
            buildings[static_cast<size_t>(hint_idx)].node == node) {
            return hint_idx;
        }
        for (int i = 0; i < static_cast<int>(buildings.size()); ++i) {
            if (buildings[static_cast<size_t>(i)].node == node) return i;
        }
        return -1;
    }

    void SpawnPlaneStrike(int b_idx) {
        if (b_idx < 0 || b_idx >= static_cast<int>(buildings.size())) return;
        auto& b = buildings[static_cast<size_t>(b_idx)];
        if (b.strike_incoming || b.collapsing || b.demolished) return;

        b.strike_incoming = true;

        float cx = (b.x0 + b.x1) * 0.5f;
        float cz = (b.z0 + b.z1) * 0.5f;
        float bw = b.x1 - b.x0;
        float bd = b.z1 - b.z0;
        float imp_y = std::clamp(b.target_height * 0.68f, 1.8f, std::max(2.0f, b.target_height - 0.8f));

        // Choose an approach azimuth angled ~40 deg to camera forward so the aircraft profile is dramatic
        DirectX::XMFLOAT3 cam_fwd = cam.GetForward();
        float base_ang = std::atan2(cam_fwd.z, cam_fwd.x) + ((strikes.size() % 2 == 0) ? 0.88f : -0.88f);
        DirectX::XMFLOAT3 approach_dir{std::cos(base_ang), 0.0f, std::sin(base_ang)};

        float half_span = 0.45f * (std::abs(approach_dir.x) * bw + std::abs(approach_dir.z) * bd);
        DirectX::XMFLOAT3 impact_pos{
            cx - approach_dir.x * half_span,
            imp_y,
            cz - approach_dir.z * half_span
        };

        float travel_dist = 185.0f;
        DirectX::XMFLOAT3 start_pos{
            impact_pos.x - approach_dir.x * travel_dist,
            imp_y + 26.0f,
            impact_pos.z - approach_dir.z * travel_dist
        };

        city::PlaneStrike ps{};
        ps.building_idx = b_idx;
        ps.target_node = b.node;
        ps.building_id = b.id;
        ps.start_pos = start_pos;
        ps.impact_pos = impact_pos;
        ps.pos = start_pos;
        ps.dir = approach_dir;
        ps.yaw = std::atan2(approach_dir.x, approach_dir.z);
        ps.pitch = 0.14f;
        ps.roll = 0.0f;
        ps.duration = 3.60f;
        ps.scale = std::clamp(0.85f + std::min(bw, bd) * 0.025f, 0.85f, 1.25f);
        strikes.push_back(ps);

        FrameStrikeView(b, approach_dir);
    }

    void TriggerImpactExplosion(city::Building& b, const city::PlaneStrike& ps) {
        b.strike_incoming = false;
        b.collapsing = true;
        b.collapse_timer = 0.0f;
        b.impact_y = ps.impact_pos.y;
        b.impact_dir_x = ps.dir.x;
        b.impact_dir_z = ps.dir.z;
        b.burn_intensity = 1.0f;
        static_mesh_epoch++;

        cam.shake_trauma = std::min(1.0f, cam.shake_trauma + 0.92f);

        float cx = (b.x0 + b.x1) * 0.5f;
        float cz = (b.z0 + b.z1) * 0.5f;
        float span = std::max(b.x1 - b.x0, b.z1 - b.z0);
        DirectX::XMFLOAT3 exit_pos{
            cx + ps.dir.x * (span * 0.55f),
            ps.impact_pos.y,
            cz + ps.dir.z * (span * 0.55f)
        };

        std::uniform_real_distribution<float> spread(-1.0f, 1.0f);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);

        // 1. Expanding horizontal shockwave ring
        {
            city::Particle3D sw{};
            sw.kind = city::Particle3D::Kind::Shockwave;
            sw.pos = {cx, ps.impact_pos.y, cz};
            sw.color = {1.0f, 0.72f, 0.38f, 0.85f};
            sw.size = span * 0.55f;
            sw.end_size = span * 3.2f + 16.0f;
            sw.life = 0.55f;
            sw.max_life = 0.55f;
            particles.push_back(sw);
        }

        // 2. Massive Fireball (entry + exit plumes)
        for (int i = 0; i < 42; ++i) {
            bool on_exit = (i % 3 == 0);
            DirectX::XMFLOAT3 orig = on_exit ? exit_pos : ps.impact_pos;
            float dir_sign = on_exit ? 1.0f : -0.45f;

            city::Particle3D p{};
            p.kind = city::Particle3D::Kind::Fire;
            p.pos = {
                orig.x + spread(rng) * 1.8f,
                orig.y + spread(rng) * 1.6f,
                orig.z + spread(rng) * 1.8f
            };
            p.vel = {
                ps.dir.x * dir_sign * (12.0f + unit(rng) * 24.0f) + spread(rng) * 9.0f,
                4.0f + unit(rng) * 14.0f,
                ps.dir.z * dir_sign * (12.0f + unit(rng) * 24.0f) + spread(rng) * 9.0f
            };
            p.rot = {unit(rng) * 6.28f, unit(rng) * 6.28f, unit(rng) * 6.28f};
            p.rot_vel = {spread(rng) * 3.0f, spread(rng) * 3.0f, spread(rng) * 3.0f};
            p.color = (i % 4 == 0) ? DirectX::XMFLOAT4{1.0f, 0.94f, 0.65f, 0.95f}
                                   : DirectX::XMFLOAT4{1.0f, 0.42f, 0.10f, 0.92f};
            p.size = 2.2f + unit(rng) * 2.4f;
            p.end_size = p.size * 2.4f;
            p.life = 0.45f + unit(rng) * 0.55f;
            p.max_life = p.life;
            particles.push_back(p);
        }

        // 3. Hot sparks & burning plane fragments shooting through the tower
        for (int i = 0; i < 36; ++i) {
            city::Particle3D p{};
            p.kind = city::Particle3D::Kind::Spark;
            p.pos = exit_pos;
            p.vel = {
                ps.dir.x * (22.0f + unit(rng) * 35.0f) + spread(rng) * 16.0f,
                2.0f + unit(rng) * 18.0f,
                ps.dir.z * (22.0f + unit(rng) * 35.0f) + spread(rng) * 16.0f
            };
            p.color = {1.0f, 0.78f, 0.25f, 1.0f};
            p.size = 0.45f + unit(rng) * 0.4f;
            p.end_size = 0.15f;
            p.life = 0.65f + unit(rng) * 0.75f;
            p.max_life = p.life;
            particles.push_back(p);
        }

        // 4. Initial dark smoke billows
        for (int i = 0; i < 24; ++i) {
            city::Particle3D p{};
            p.kind = city::Particle3D::Kind::Smoke;
            p.pos = {
                cx + spread(rng) * (span * 0.45f),
                ps.impact_pos.y + spread(rng) * 1.5f,
                cz + spread(rng) * (span * 0.45f)
            };
            p.vel = {
                spread(rng) * 6.5f + ps.dir.x * 5.0f,
                6.0f + unit(rng) * 9.0f,
                spread(rng) * 6.5f + ps.dir.z * 5.0f
            };
            p.rot = {unit(rng) * 6.28f, unit(rng) * 6.28f, unit(rng) * 6.28f};
            p.rot_vel = {spread(rng) * 1.5f, spread(rng) * 1.5f, spread(rng) * 1.5f};
            float shade = 0.14f + unit(rng) * 0.12f;
            p.color = {shade, shade * 1.02f, shade * 1.08f, 0.78f};
            p.size = 2.4f + unit(rng) * 2.0f;
            p.end_size = p.size * 3.2f;
            p.life = 1.8f + unit(rng) * 1.4f;
            p.max_life = p.life;
            particles.push_back(p);
        }

        // 5. Concrete & steel debris chunks
        for (int i = 0; i < 32; ++i) {
            city::Particle3D p{};
            p.kind = city::Particle3D::Kind::Debris;
            p.pos = {
                cx + spread(rng) * (span * 0.5f),
                ps.impact_pos.y + spread(rng) * 2.5f,
                cz + spread(rng) * (span * 0.5f)
            };
            p.vel = {
                spread(rng) * 18.0f + ps.dir.x * 12.0f,
                4.0f + unit(rng) * 16.0f,
                spread(rng) * 18.0f + ps.dir.z * 12.0f
            };
            p.rot = {unit(rng) * 6.28f, unit(rng) * 6.28f, unit(rng) * 6.28f};
            p.rot_vel = {spread(rng) * 7.0f, spread(rng) * 7.0f, spread(rng) * 7.0f};
            p.color = (i % 4 == 0) ? DirectX::XMFLOAT4{0.92f, 0.36f, 0.14f, 1.0f}
                                   : DirectX::XMFLOAT4{0.26f, 0.30f, 0.36f, 1.0f};
            p.size = 0.65f + unit(rng) * 0.75f;
            p.end_size = p.size;
            p.life = 1.6f + unit(rng) * 1.0f;
            p.max_life = p.life;
            particles.push_back(p);
        }
    }

    void ShowToast(const std::string& msg, bool is_err = false) {
        toast_msg = msg;
        toast_is_error = is_err;
        toast_max_timer = 4.5f;
        toast_timer = toast_max_timer;
    }
};

AppState* g_app = nullptr;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
        return TRUE;
    }

    switch (msg) {
        case WM_SIZE:
            if (g_app && wParam != SIZE_MINIMIZED) {
                int w = LOWORD(lParam);
                int h = HIWORD(lParam);
                g_app->renderer.Resize(w, h);
            }
            return 0;

        case WM_RBUTTONDOWN:
            if (g_app && !ImGui::GetIO().WantCaptureMouse) {
                g_app->rmb_looking = true;
                g_app->ui.orbit_mode = false;
                GetCursorPos(&g_app->last_mouse);
                SetCapture(hwnd);
            }
            return 0;

        case WM_RBUTTONUP:
            if (g_app && g_app->rmb_looking) {
                g_app->rmb_looking = false;
                ReleaseCapture();
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (g_app && !ImGui::GetIO().WantCaptureMouse) {
                POINT pt{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
                int hit = g_app->renderer.PickBuilding(g_app->buildings, g_app->cam,
                                                       static_cast<float>(pt.x), static_cast<float>(pt.y));
                g_app->selected_idx = hit;
            }
            return 0;

        case WM_LBUTTONDBLCLK:
            if (g_app && !ImGui::GetIO().WantCaptureMouse) {
                POINT pt{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
                int hit = g_app->renderer.PickBuilding(g_app->buildings, g_app->cam,
                                                       static_cast<float>(pt.x), static_cast<float>(pt.y));
                if (hit >= 0 && hit < static_cast<int>(g_app->buildings.size())) {
                    auto* node = g_app->buildings[static_cast<size_t>(hit)].node;
                    if (node && node->is_dir) {
                        g_app->EnterDirectory(node);
                    }
                }
            }
            return 0;

        case WM_MOUSEWHEEL:
            if (g_app && !ImGui::GetIO().WantCaptureMouse) {
                float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / 120.0f;
                DirectX::XMFLOAT3 fwd = g_app->cam.GetForward();
                g_app->ui.orbit_mode = false;
                g_app->cam.flying_to_target = false;
                g_app->cam.vel.x += fwd.x * delta * 36.0f;
                g_app->cam.vel.y += fwd.y * delta * 36.0f;
                g_app->cam.vel.z += fwd.z * delta * 36.0f;
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ApplyPremiumTheme() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 12.0f;
    st.ChildRounding = 9.0f;
    st.FrameRounding = 7.0f;
    st.PopupRounding = 10.0f;
    st.ScrollbarRounding = 8.0f;
    st.GrabRounding = 7.0f;
    st.WindowBorderSize = 1.0f;
    st.ChildBorderSize = 1.0f;
    st.FrameBorderSize = 1.0f;
    st.ItemSpacing = ImVec2(8.0f, 7.0f);
    st.WindowPadding = ImVec2(14.0f, 11.0f);
    st.FramePadding = ImVec2(10.0f, 5.0f);
    st.ScrollbarSize = 10.0f;

    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.039f, 0.055f, 0.094f, 0.88f);
    c[ImGuiCol_ChildBg]          = ImVec4(0.055f, 0.078f, 0.130f, 0.70f);
    c[ImGuiCol_PopupBg]          = ImVec4(0.045f, 0.064f, 0.110f, 0.96f);
    c[ImGuiCol_Border]           = ImVec4(0.220f, 0.480f, 0.780f, 0.34f);
    c[ImGuiCol_BorderShadow]     = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.078f, 0.110f, 0.180f, 0.85f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.115f, 0.165f, 0.270f, 0.95f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.145f, 0.215f, 0.350f, 1.00f);
    c[ImGuiCol_TitleBg]          = ImVec4(0.039f, 0.055f, 0.094f, 0.96f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.055f, 0.085f, 0.145f, 0.98f);
    c[ImGuiCol_Button]           = ImVec4(0.095f, 0.165f, 0.280f, 0.86f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.145f, 0.275f, 0.460f, 0.96f);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.185f, 0.365f, 0.620f, 1.00f);
    c[ImGuiCol_Header]           = ImVec4(0.120f, 0.240f, 0.420f, 0.72f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.165f, 0.330f, 0.560f, 0.88f);
    c[ImGuiCol_HeaderActive]     = ImVec4(0.200f, 0.410f, 0.680f, 1.00f);
    c[ImGuiCol_Separator]        = ImVec4(0.220f, 0.440f, 0.720f, 0.28f);
    c[ImGuiCol_ScrollbarBg]      = ImVec4(0.030f, 0.045f, 0.075f, 0.45f);
    c[ImGuiCol_ScrollbarGrab]    = ImVec4(0.180f, 0.340f, 0.560f, 0.65f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.240f, 0.460f, 0.740f, 0.85f);
    c[ImGuiCol_Text]             = ImVec4(0.940f, 0.965f, 0.995f, 1.00f);
    c[ImGuiCol_TextDisabled]     = ImVec4(0.510f, 0.585f, 0.700f, 1.00f);
    c[ImGuiCol_PlotHistogram]    = ImVec4(0.220f, 0.780f, 0.980f, 1.00f);
}

void UpdateCameraAndPhysics(AppState& app, float dt, float time_sec) {
    ImGuiIO& io = ImGui::GetIO();

    // Mouse look with RMB
    if (app.rmb_looking) {
        POINT cur{};
        GetCursorPos(&cur);
        float dx = static_cast<float>(cur.x - app.last_mouse.x);
        float dy = static_cast<float>(cur.y - app.last_mouse.y);
        app.last_mouse = cur;

        app.cam.flying_to_target = false;
        app.ui.orbit_mode = false;
        app.cam.yaw += dx * 0.0045f;
        app.cam.pitch = std::clamp(app.cam.pitch + dy * 0.0045f, -1.2f, 1.45f);
    }

    // Smooth inertia WASD fly controls
    DirectX::XMFLOAT3 wish{0.0f, 0.0f, 0.0f};
    if (!io.WantCaptureKeyboard) {
        auto key_down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        float speed = key_down(VK_SHIFT) ? 105.0f : 48.0f;
        DirectX::XMFLOAT3 fwd = app.cam.GetForward();
        DirectX::XMFLOAT3 right = app.cam.GetRight();
        bool moved = false;

        if (key_down('W') || key_down(VK_UP)) {
            wish.x += fwd.x * speed; wish.y += fwd.y * speed; wish.z += fwd.z * speed; moved = true;
        }
        if (key_down('S') || key_down(VK_DOWN)) {
            wish.x -= fwd.x * speed; wish.y -= fwd.y * speed; wish.z -= fwd.z * speed; moved = true;
        }
        if (key_down('D') || key_down(VK_RIGHT)) {
            wish.x += right.x * speed; wish.z += right.z * speed; moved = true;
        }
        if (key_down('A') || key_down(VK_LEFT)) {
            wish.x -= right.x * speed; wish.z -= right.z * speed; moved = true;
        }
        if (key_down('E') || key_down(VK_SPACE)) {
            wish.y += speed; moved = true;
        }
        if (key_down('Q') || key_down(VK_CONTROL)) {
            wish.y -= speed; moved = true;
        }
        if (moved) {
            app.cam.flying_to_target = false;
            app.ui.orbit_mode = false;
        }

        static bool prev_bs = false;
        bool cur_bs = key_down(VK_BACK);
        if (cur_bs && !prev_bs) app.GoUp();
        prev_bs = cur_bs;

        static bool prev_r = false;
        bool cur_r = key_down('R');
        if (cur_r && !prev_r) app.ResetCamera();
        prev_r = cur_r;

        static bool prev_o = false;
        bool cur_o = key_down('O');
        if (cur_o && !prev_o) {
            app.ui.orbit_mode = !app.ui.orbit_mode;
            app.cam.flying_to_target = false;
            if (app.ui.orbit_mode) {
                app.ui.orbit_angle = std::atan2(app.cam.pos.z, app.cam.pos.x);
            }
        }
        prev_o = cur_o;

        static bool prev_del = false;
        bool cur_del = key_down(VK_DELETE);
        if (cur_del && !prev_del && app.root_tree && app.selected_idx >= 0 && app.selected_idx < static_cast<int>(app.buildings.size())) {
            auto& b = app.buildings[static_cast<size_t>(app.selected_idx)];
            if (b.node && !b.is_protected && !b.collapsing && !b.strike_incoming && !b.demolished) {
                app.pending_delete_node = b.node;
                app.pending_delete_building_idx = app.selected_idx;
                app.confirm_delete_open = true;
            }
        }
        prev_del = cur_del;
    }

    float vel_damp = 1.0f - std::exp(-10.0f * dt);
    app.cam.vel.x += (wish.x - app.cam.vel.x) * vel_damp;
    app.cam.vel.y += (wish.y - app.cam.vel.y) * vel_damp;
    app.cam.vel.z += (wish.z - app.cam.vel.z) * vel_damp;

    if (!app.cam.flying_to_target && !app.ui.orbit_mode) {
        app.cam.pos.x += app.cam.vel.x * dt;
        app.cam.pos.y = std::clamp(app.cam.pos.y + app.cam.vel.y * dt, 2.5f, 250.0f);
        app.cam.pos.z += app.cam.vel.z * dt;
    }

    // Optional smooth cinematic orbit
    if (app.ui.orbit_mode) {
        app.ui.orbit_angle += dt * 0.16f;
        float radius = 108.0f;
        float ty = 64.0f + std::sin(time_sec * 0.4f) * 6.0f;
        DirectX::XMFLOAT3 goal{std::cos(app.ui.orbit_angle) * radius, ty, std::sin(app.ui.orbit_angle) * radius};
        float k = 1.0f - std::exp(-3.5f * dt);
        app.cam.pos.x += (goal.x - app.cam.pos.x) * k;
        app.cam.pos.y += (goal.y - app.cam.pos.y) * k;
        app.cam.pos.z += (goal.z - app.cam.pos.z) * k;
        app.cam.yaw = std::atan2(-app.cam.pos.x, -app.cam.pos.z);
        app.cam.pitch = 0.44f;
    }

    // Smooth camera fly-to interpolation
    if (app.cam.flying_to_target) {
        float k = 1.0f - std::exp(-6.5f * dt);
        app.cam.pos.x += (app.cam.target_pos.x - app.cam.pos.x) * k;
        app.cam.pos.y += (app.cam.target_pos.y - app.cam.pos.y) * k;
        app.cam.pos.z += (app.cam.target_pos.z - app.cam.pos.z) * k;

        // Shortest angular interpolation for yaw
        float dyaw = app.cam.target_yaw - app.cam.yaw;
        while (dyaw > 3.14159265f) dyaw -= 6.28318530f;
        while (dyaw < -3.14159265f) dyaw += 6.28318530f;
        app.cam.yaw += dyaw * k;
        app.cam.pitch += (app.cam.target_pitch - app.cam.pitch) * k;

        float d2 = (app.cam.target_pos.x - app.cam.pos.x) * (app.cam.target_pos.x - app.cam.pos.x) +
                   (app.cam.target_pos.y - app.cam.pos.y) * (app.cam.target_pos.y - app.cam.pos.y) +
                   (app.cam.target_pos.z - app.cam.pos.z) * (app.cam.target_pos.z - app.cam.pos.z);
        if (d2 < 0.04f && std::abs(dyaw) < 0.01f) app.cam.flying_to_target = false;
    }

    // Camera impact trauma shake
    if (app.cam.shake_trauma > 0.001f) {
        float shake = app.cam.shake_trauma * app.cam.shake_trauma;
        app.cam.shake_offset.x = std::sin(time_sec * 57.0f) * shake * 1.65f;
        app.cam.shake_offset.y = std::cos(time_sec * 49.0f) * shake * 1.45f;
        app.cam.shake_offset.z = std::sin(time_sec * 63.0f + 1.3f) * shake * 1.35f;
        app.cam.shake_trauma = std::max(0.0f, app.cam.shake_trauma - dt * 0.95f);
    } else {
        app.cam.shake_offset = {0.0f, 0.0f, 0.0f};
    }

    // Hover picking
    if (!io.WantCaptureMouse && !app.rmb_looking) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(app.hwnd, &pt);
        app.hovered_idx = app.renderer.PickBuilding(app.buildings, app.cam,
                                                    static_cast<float>(pt.x), static_cast<float>(pt.y));
    } else {
        app.hovered_idx = -1;
    }

    // Traffic animation
    for (auto& car : app.cars) {
        car.axis_pos += static_cast<float>(car.dir) * car.speed * dt;
        if (car.axis_pos > 112.0f) car.axis_pos = -112.0f;
        if (car.axis_pos < -112.0f) car.axis_pos = 112.0f;
    }

    // Plane Strikes flight & impact trigger
    std::uniform_real_distribution<float> spread(-1.0f, 1.0f);
    for (auto& ps : app.strikes) {
        if (ps.finished) continue;
        if (!ps.impacted) {
            ps.progress += dt / std::max(0.2f, ps.duration);
            float t = std::clamp(ps.progress, 0.0f, 1.0f);

            // Smooth banking alignment turn on approach
            float lateral_s = std::sin(t * 3.14159265f) * (1.0f - t) * 14.0f;
            DirectX::XMFLOAT3 side{-ps.dir.z, 0.0f, ps.dir.x};
            DirectX::XMFLOAT3 prev_pos = ps.pos;

            ps.pos.x = ps.start_pos.x + (ps.impact_pos.x - ps.start_pos.x) * t + side.x * lateral_s;
            ps.pos.y = ps.start_pos.y + (ps.impact_pos.y - ps.start_pos.y) * t + std::sin(t * 3.14159265f) * 2.5f;
            ps.pos.z = ps.start_pos.z + (ps.impact_pos.z - ps.start_pos.z) * t + side.z * lateral_s;

            DirectX::XMFLOAT3 vel_dir{ps.pos.x - prev_pos.x, ps.pos.y - prev_pos.y, ps.pos.z - prev_pos.z};
            float vl = std::sqrt(vel_dir.x * vel_dir.x + vel_dir.y * vel_dir.y + vel_dir.z * vel_dir.z);
            if (vl > 1e-4f) {
                ps.yaw = std::atan2(vel_dir.x, vel_dir.z);
                float horiz = std::sqrt(vel_dir.x * vel_dir.x + vel_dir.z * vel_dir.z);
                ps.pitch = -std::atan2(vel_dir.y, std::max(1e-3f, horiz));
            }
            // Dramatic bank roll during mid-approach, leveling out near impact
            ps.roll = std::sin(t * 3.14159265f) * 0.42f * (1.0f - t * 0.65f);

            // Engine contrails
            ps.trail_accum += dt;
            if (ps.trail_accum >= 0.025f) {
                ps.trail_accum = 0.0f;
                for (float sgn : {-1.0f, 1.0f}) {
                    city::Particle3D tr{};
                    tr.kind = city::Particle3D::Kind::Smoke;
                    tr.pos = {
                        ps.pos.x - ps.dir.x * 2.5f + side.x * (sgn * 2.5f * ps.scale),
                        ps.pos.y - 0.6f,
                        ps.pos.z - ps.dir.z * 2.5f + side.z * (sgn * 2.5f * ps.scale)
                    };
                    tr.vel = {spread(app.rng) * 0.8f, 0.5f, spread(app.rng) * 0.8f};
                    tr.color = {0.72f, 0.78f, 0.86f, 0.34f};
                    tr.size = 0.65f;
                    tr.end_size = 2.8f;
                    tr.life = 2.20f;
                    tr.max_life = 2.20f;
                    app.particles.push_back(tr);
                }
            }

            if (ps.progress >= 1.0f) {
                ps.impacted = true;
                int b_idx = app.FindBuildingIndex(ps.target_node, ps.building_idx);
                if (b_idx >= 0 && b_idx < static_cast<int>(app.buildings.size())) {
                    app.TriggerImpactExplosion(app.buildings[static_cast<size_t>(b_idx)], ps);
                }
                ps.finished = true;
            }
        }
    }
    app.strikes.erase(
        std::remove_if(app.strikes.begin(), app.strikes.end(), [](const city::PlaneStrike& s) { return s.finished; }),
        app.strikes.end()
    );

    // Building spawn rise & progressive pancake collapse animations
    bool any_settled_now = false;
    for (auto& b : app.buildings) {
        if (b.demolished) continue;

        if (b.collapsing) {
            b.collapse_timer += dt;
            float cx = (b.x0 + b.x1) * 0.5f;
            float cz = (b.z0 + b.z1) * 0.5f;
            float span = std::max(b.x1 - b.x0, b.z1 - b.z0);

            // Phase A (0 .. 0.85s): Upper block leans/tilts while breach burns intensely
            // Phase B (0.85 .. 2.75s): Progressive pancake collapse downward into dust & embers
            if (b.collapse_timer < 0.85f) {
                float u = b.collapse_timer / 0.85f;
                b.upper_tilt = 0.24f * (u * u);
                b.upper_drop = 0.0f;
                b.lower_scale = 1.0f;
                b.burn_intensity = 1.0f;
                b.shake_offset_x = std::sin(b.collapse_timer * 75.0f) * 0.35f;
                b.shake_offset_z = std::cos(b.collapse_timer * 68.0f) * 0.35f;

                // Continuous fire & thick smoke from impact breach
                if (std::uniform_real_distribution<float>(0.0f, 1.0f)(app.rng) < 0.65f) {
                    city::Particle3D sm{};
                    sm.kind = city::Particle3D::Kind::Smoke;
                    sm.pos = {cx + spread(app.rng) * (span * 0.35f), b.impact_y, cz + spread(app.rng) * (span * 0.35f)};
                    sm.vel = {spread(app.rng) * 3.5f, 6.5f + std::abs(spread(app.rng)) * 6.0f, spread(app.rng) * 3.5f};
                    sm.color = {0.16f, 0.17f, 0.19f, 0.72f};
                    sm.size = 2.0f;
                    sm.end_size = 6.5f;
                    sm.life = 1.9f;
                    sm.max_life = 1.9f;
                    app.particles.push_back(sm);
                }
            } else if (b.collapse_timer < 2.75f) {
                float c = (b.collapse_timer - 0.85f) / (2.75f - 0.85f);
                float c2 = c * c;
                b.upper_tilt = 0.24f + 0.26f * c;
                b.lower_scale = std::max(0.0f, 1.0f - c2);
                b.upper_drop = (b.target_height - b.impact_y + 2.0f) * c2;
                b.burn_intensity = std::max(0.0f, 1.0f - c * 0.8f);
                b.shake_offset_x = std::sin(b.collapse_timer * 82.0f) * 0.45f * (1.0f - c);
                b.shake_offset_z = std::cos(b.collapse_timer * 74.0f) * 0.45f * (1.0f - c);

                float front_y = std::max(0.6f, b.impact_y * b.lower_scale);
                app.cam.shake_trauma = std::max(app.cam.shake_trauma, 0.26f * (1.0f - c));

                // Dust clouds & concrete debris cascading at the collapse front
                for (int k = 0; k < 2; ++k) {
                    city::Particle3D dust{};
                    dust.kind = city::Particle3D::Kind::Smoke;
                    dust.pos = {cx + spread(app.rng) * (span * 0.55f), front_y + spread(app.rng) * 1.2f, cz + spread(app.rng) * (span * 0.55f)};
                    dust.vel = {spread(app.rng) * 8.5f, 2.5f + std::abs(spread(app.rng)) * 5.0f, spread(app.rng) * 8.5f};
                    float g = 0.34f + std::abs(spread(app.rng)) * 0.14f;
                    dust.color = {g, g * 1.02f, g * 1.06f, 0.58f};
                    dust.size = 2.6f;
                    dust.end_size = 8.5f;
                    dust.life = 1.6f;
                    dust.max_life = 1.6f;
                    app.particles.push_back(dust);
                }
                if (std::uniform_real_distribution<float>(0.0f, 1.0f)(app.rng) < 0.55f) {
                    city::Particle3D deb{};
                    deb.kind = city::Particle3D::Kind::Debris;
                    deb.pos = {cx + spread(app.rng) * (span * 0.45f), front_y, cz + spread(app.rng) * (span * 0.45f)};
                    deb.vel = {spread(app.rng) * 12.0f, 2.0f + std::abs(spread(app.rng)) * 8.0f, spread(app.rng) * 12.0f};
                    deb.rot_vel = {spread(app.rng) * 6.0f, spread(app.rng) * 6.0f, spread(app.rng) * 6.0f};
                    deb.color = {0.25f, 0.28f, 0.34f, 1.0f};
                    deb.size = 0.65f;
                    deb.end_size = 0.65f;
                    deb.life = 1.3f;
                    deb.max_life = 1.3f;
                    app.particles.push_back(deb);
                }
            } else {
                b.demolished = true;
                b.collapsing = false;
                b.height = 0.0f;
                b.burn_intensity = 0.0f;
                any_settled_now = true;
            }
        } else if (b.spawn_progress < 1.0f) {
            b.spawn_timer += dt;
            if (b.spawn_timer > b.spawn_delay) {
                float u = std::clamp((b.spawn_timer - b.spawn_delay) / 0.55f, 0.0f, 1.0f);
                b.spawn_progress = u;
                b.height = std::max(0.15f, b.target_height * EaseOutBack(u));
                if (u >= 1.0f) {
                    b.height = b.target_height;
                    any_settled_now = true;
                }
            }
        } else if (std::abs(b.height - b.target_height) > 0.01f) {
            b.height += (b.target_height - b.height) * std::min(1.0f, dt * 8.0f);
        }
    }
    if (any_settled_now) {
        app.static_mesh_epoch++;
    }

    // Particle physics
    for (auto& p : app.particles) {
        p.pos.x += p.vel.x * dt;
        p.pos.y += p.vel.y * dt;
        p.pos.z += p.vel.z * dt;
        p.rot.x += p.rot_vel.x * dt;
        p.rot.y += p.rot_vel.y * dt;
        p.rot.z += p.rot_vel.z * dt;

        if (p.kind == city::Particle3D::Kind::Debris || p.kind == city::Particle3D::Kind::Spark) {
            p.vel.y -= 36.0f * dt;
            if (p.pos.y < p.size * 0.4f) {
                p.pos.y = p.size * 0.4f;
                p.vel.y = -p.vel.y * 0.38f;
                p.vel.x *= 0.72f;
                p.vel.z *= 0.72f;
            }
        } else if (p.kind == city::Particle3D::Kind::Smoke) {
            p.vel.x *= std::exp(-1.4f * dt);
            p.vel.z *= std::exp(-1.4f * dt);
            p.vel.y += 1.8f * dt;
        } else if (p.kind == city::Particle3D::Kind::Fire) {
            p.vel.x *= std::exp(-2.8f * dt);
            p.vel.z *= std::exp(-2.8f * dt);
            p.vel.y += 4.5f * dt;
        }
        p.life -= dt;
    }
    app.particles.erase(
        std::remove_if(app.particles.begin(), app.particles.end(), [](const city::Particle3D& p) { return p.life <= 0.0f; }),
        app.particles.end()
    );

    // UI smooth animations
    app.ui.displayed_freed_bytes += (static_cast<double>(app.session_freed_bytes) - app.ui.displayed_freed_bytes) * (1.0 - std::exp(-9.0 * dt));
    app.ui.freed_flash = std::max(0.0f, app.ui.freed_flash - dt * 1.4f);

    float target_mode = (app.view_mode == ViewMode::TopJunk) ? 1.0f : 0.0f;
    app.ui.mode_pill_t += (target_mode - app.ui.mode_pill_t) * (1.0f - std::exp(-14.0f * dt));

    float target_insp = (app.selected_idx >= 0 && app.selected_idx < static_cast<int>(app.buildings.size())) ? 1.0f : 0.0f;
    app.ui.inspector_reveal += (target_insp - app.ui.inspector_reveal) * (1.0f - std::exp(-12.0f * dt));

    if (app.toast_timer > 0.0f) {
        app.toast_timer = std::max(0.0f, app.toast_timer - dt);
        float target_ta = (app.toast_timer > 0.35f) ? 1.0f : (app.toast_timer / 0.35f);
        app.ui.toast_anim += (target_ta - app.ui.toast_anim) * (1.0f - std::exp(-16.0f * dt));
    } else {
        app.ui.toast_anim = 0.0f;
    }
}

void Draw3DLabelsAndStrikeHUD(const AppState& app, float inspector_left_x, float time_sec) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    struct LabelRect { float x0, y0, x1, y1; };
    std::vector<LabelRect> placed_rects;

    // 1. Target Lock HUD reticle for incoming plane strikes
    for (const auto& ps : app.strikes) {
        if (ps.impacted || ps.finished) continue;
        int b_idx = app.FindBuildingIndex(ps.target_node, ps.building_idx);
        if (b_idx < 0 || b_idx >= static_cast<int>(app.buildings.size())) continue;
        const auto& b = app.buildings[static_cast<size_t>(b_idx)];

        float sx = 0.0f, sy = 0.0f;
        if (app.renderer.ProjectToScreen(ps.impact_pos, app.cam, sx, sy)) {
            float pulse = 1.0f + 0.14f * std::sin(time_sec * 18.0f);
            float rad = 24.0f * pulse;
            ImU32 col = IM_COL32(255, 75, 55, 235);
            dl->AddCircle(ImVec2(sx, sy), rad, col, 24, 2.0f);
            dl->AddLine(ImVec2(sx - rad - 7, sy), ImVec2(sx - rad + 5, sy), col, 2.0f);
            dl->AddLine(ImVec2(sx + rad - 5, sy), ImVec2(sx + rad + 7, sy), col, 2.0f);
            dl->AddLine(ImVec2(sx, sy - rad - 7), ImVec2(sx, sy - rad + 5), col, 2.0f);
            dl->AddLine(ImVec2(sx, sy + rad - 5), ImVec2(sx, sy + rad + 7), col, 2.0f);

            float rem = std::max(0.0f, (1.0f - ps.progress) * ps.duration);
            char tag[64]{};
            snprintf(tag, sizeof(tag), "TARGET: %s · %.1fs", b.label.c_str(), rem);
            ImVec2 tsz = ImGui::CalcTextSize(tag);
            ImVec2 tp0(sx - tsz.x * 0.5f - 8.0f, sy + rad + 8.0f);
            ImVec2 tp1(sx + tsz.x * 0.5f + 8.0f, sy + rad + 8.0f + tsz.y + 6.0f);
            dl->AddRectFilled(tp0, tp1, IM_COL32(45, 10, 12, 225), 6.0f);
            dl->AddRect(tp0, tp1, col, 6.0f, 0, 1.5f);
            dl->AddText(ImVec2(tp0.x + 8.0f, tp0.y + 3.0f), IM_COL32(255, 220, 210, 255), tag);
        }
    }

    // 2. Floating Skyscraper Callout Labels
    std::vector<int> order;
    if (app.selected_idx >= 0) order.push_back(app.selected_idx);
    if (app.hovered_idx >= 0 && app.hovered_idx != app.selected_idx) order.push_back(app.hovered_idx);
    for (int i = 0; i < static_cast<int>(app.buildings.size()) && i < 16; ++i) {
        if (i != app.selected_idx && i != app.hovered_idx) order.push_back(i);
    }

    for (int i : order) {
        if (i < 0 || i >= static_cast<int>(app.buildings.size())) continue;
        const auto& b = app.buildings[static_cast<size_t>(i)];
        if (b.collapsing || b.demolished || b.strike_incoming || b.height <= 0.3f) continue;

        float cx = (b.x0 + b.x1) * 0.5f;
        float cz = (b.z0 + b.z1) * 0.5f;
        float rx = 0.0f, ry = 0.0f;
        float sx = 0.0f, sy = 0.0f;
        if (!app.renderer.ProjectToScreen({cx, b.height + 0.6f, cz}, app.cam, rx, ry)) continue;
        if (!app.renderer.ProjectToScreen({cx, b.height + 3.4f, cz}, app.cam, sx, sy)) continue;
        if (sy < 118.0f || sy > static_cast<float>(app.renderer.Height()) - 52.0f) continue;
        if (sx < 36.0f || sx > inspector_left_x - 36.0f) continue;

        std::string text = b.label;
        if (text.size() > 20) text = text.substr(0, 18) + "..";
        text += " · " + b.size_label;

        ImVec2 tsz = ImGui::CalcTextSize(text.c_str());
        float pad_l = 18.0f;
        float pad_r = 8.0f;
        ImVec2 p0(sx - (tsz.x + pad_l + pad_r) * 0.5f, sy - tsz.y * 0.5f - 4.0f);
        ImVec2 p1(sx + (tsz.x + pad_l + pad_r) * 0.5f, sy + tsz.y * 0.5f + 4.0f);

        bool overlaps = false;
        if (i != app.selected_idx && i != app.hovered_idx) {
            for (const auto& r : placed_rects) {
                if (!(p1.x + 5.0f < r.x0 || p0.x - 5.0f > r.x1 || p1.y + 5.0f < r.y0 || p0.y - 5.0f > r.y1)) {
                    overlaps = true;
                    break;
                }
            }
        }
        if (overlaps) continue;
        placed_rects.push_back({p0.x, p0.y, p1.x, p1.y});

        ImU32 catCol = CategoryBadgeColor(b.category, b.is_protected);
        ImU32 bg = (i == app.selected_idx) ? IM_COL32(36, 28, 12, 238)
                 : (i == app.hovered_idx)  ? IM_COL32(16, 30, 52, 238)
                                           : IM_COL32(10, 15, 26, 212);
        ImU32 bd = (i == app.selected_idx) ? IM_COL32(255, 212, 68, 255)
                 : (i == app.hovered_idx)  ? IM_COL32(105, 215, 255, 255)
                                           : catCol;

        // Subtle leader line from roof to callout pill
        dl->AddLine(ImVec2(rx, ry), ImVec2(sx, p1.y), (i == app.selected_idx || i == app.hovered_idx) ? bd : IM_COL32(120, 160, 210, 110), 1.2f);
        dl->AddCircleFilled(ImVec2(rx, ry), 2.5f, bd);

        dl->AddRectFilled(ImVec2(p0.x + 2, p0.y + 2), ImVec2(p1.x + 2, p1.y + 2), IM_COL32(0, 0, 0, 110), 7.0f);
        dl->AddRectFilled(p0, p1, bg, 7.0f);
        dl->AddRect(p0, p1, bd, 7.0f, 0, (i == app.selected_idx || i == app.hovered_idx) ? 1.8f : 1.0f);
        dl->AddCircleFilled(ImVec2(p0.x + 9.0f, (p0.y + p1.y) * 0.5f), 3.5f, catCol);
        dl->AddText(ImVec2(p0.x + pad_l, p0.y + 4.0f), IM_COL32(244, 248, 255, 255), text.c_str());
    }
}

void DrawGlassChrome(ImDrawList* dl, ImVec2 pos, ImVec2 sz, float rounding = 12.0f) {
    // Subtle top luminous accent edge inside window
    dl->AddLine(ImVec2(pos.x + rounding, pos.y + 1.0f),
                ImVec2(pos.x + sz.x - rounding, pos.y + 1.0f),
                IM_COL32(115, 200, 255, 70), 1.0f);
}

void DrawUI(AppState& app, float time_sec) {
    const float win_w = static_cast<float>(app.renderer.Width());
    const float win_h = static_cast<float>(app.renderer.Height());
    const float inspector_w = 376.0f;
    const float inspector_x = win_w - inspector_w - 14.0f;

    Draw3DLabelsAndStrikeHUD(app, inspector_x, time_sec);

    ImGuiWindowFlags bar_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar;

    // 1. Top Command Bar
    ImGui::SetNextWindowPos(ImVec2(14.0f, 12.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(win_w - 28.0f, 54.0f), ImGuiCond_Always);
    if (ImGui::Begin("##TopBar", nullptr, bar_flags)) {
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        DrawGlassChrome(wdl, ImGui::GetWindowPos(), ImGui::GetWindowSize(), 12.0f);

        // Animated status dot + Brand title
        ImVec2 cur = ImGui::GetCursorScreenPos();
        float pulse = 0.65f + 0.35f * std::sin(time_sec * 4.2f);
        ImU32 dot_col = app.scanner.GetStats().scanning ? IM_COL32(255, 200, 60, 255) : IM_COL32(55, 225, 255, 255);
        wdl->AddCircleFilled(ImVec2(cur.x + 6.0f, cur.y + 15.0f), 7.5f * pulse, IM_COL32(55, 225, 255, 45));
        wdl->AddCircleFilled(ImVec2(cur.x + 6.0f, cur.y + 15.0f), 4.0f, dot_col);
        ImGui::Dummy(ImVec2(16.0f, 0.0f));
        ImGui::SameLine();

        if (app.ui.font_bold) ImGui::PushFont(app.ui.font_bold);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.42f, 0.90f, 1.00f, 1.0f), "SSD CITY");
        if (app.ui.font_bold) ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // Drive Pills with embedded usage bar
        for (const auto& d : app.drives) {
            bool active = (app.active_scan_path == d.root_path);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.38f, 0.64f, 0.95f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.35f, 0.82f, 1.00f, 0.80f));
            }
            std::string btn_lbl = d.label_utf8 + "##drv";
            if (ImGui::Button(btn_lbl.c_str(), ImVec2(0.0f, 30.0f))) {
                app.StartScanPath(d.root_path);
            }
            if (active) ImGui::PopStyleColor(2);

            if (d.total_bytes > 0) {
                ImVec2 bmin = ImGui::GetItemRectMin();
                ImVec2 bmax = ImGui::GetItemRectMax();
                float frac = std::clamp(static_cast<float>(static_cast<double>(d.used_bytes) / static_cast<double>(d.total_bytes)), 0.0f, 1.0f);
                ImU32 bar_c = (frac > 0.88f) ? IM_COL32(255, 85, 65, 230)
                            : (frac > 0.72f) ? IM_COL32(255, 190, 55, 230)
                                             : IM_COL32(55, 210, 255, 230);
                wdl->AddRectFilled(ImVec2(bmin.x + 4.0f, bmax.y - 3.0f), ImVec2(bmax.x - 4.0f, bmax.y - 1.0f), IM_COL32(20, 30, 48, 180), 1.0f);
                wdl->AddRectFilled(ImVec2(bmin.x + 4.0f, bmax.y - 3.0f),
                                   ImVec2(bmin.x + 4.0f + (bmax.x - bmin.x - 8.0f) * frac, bmax.y - 1.0f), bar_c, 1.0f);
            }
            if (ImGui::IsItemHovered() && d.total_bytes > 0) {
                ImGui::SetTooltip("Занято %s из %s (свободно %s)",
                                  city::FormatBytesUtf8(d.used_bytes).c_str(),
                                  city::FormatBytesUtf8(d.total_bytes).c_str(),
                                  city::FormatBytesUtf8(d.free_bytes).c_str());
            }
            ImGui::SameLine();
        }

        if (ImGui::Button("Профиль", ImVec2(0.0f, 30.0f))) {
            app.StartScanPath(GetUserProfileDir());
        }
        ImGui::SameLine();
        if (ImGui::Button("Папка...", ImVec2(0.0f, 30.0f))) {
            std::wstring picked = PickFolderDialog(app.hwnd);
            if (!picked.empty()) {
                app.StartScanPath(picked);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // Animated segmented mode control: [ Город | Топ мусора ]
        {
            ImVec2 seg_pos = ImGui::GetCursorScreenPos();
            float seg_w = 210.0f;
            float seg_h = 30.0f;
            wdl->AddRectFilled(seg_pos, ImVec2(seg_pos.x + seg_w, seg_pos.y + seg_h), IM_COL32(12, 18, 32, 230), 8.0f);
            wdl->AddRect(seg_pos, ImVec2(seg_pos.x + seg_w, seg_pos.y + seg_h), IM_COL32(60, 125, 200, 95), 8.0f);

            float pill_x = seg_pos.x + 3.0f + app.ui.mode_pill_t * (seg_w * 0.5f - 3.0f);
            ImU32 pill_col = (app.ui.mode_pill_t < 0.5f) ? IM_COL32(28, 125, 108, 235) : IM_COL32(195, 62, 40, 235);
            wdl->AddRectFilled(ImVec2(pill_x, seg_pos.y + 3.0f),
                               ImVec2(pill_x + seg_w * 0.5f - 3.0f, seg_pos.y + seg_h - 3.0f), pill_col, 6.0f);

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.14f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
            if (ImGui::Button("Город##mode0", ImVec2(seg_w * 0.5f, seg_h))) {
                app.view_mode = ViewMode::Directory;
                app.RebuildCity();
                app.ResetCamera();
            }
            ImGui::SameLine(0.0f, 0.0f);
            if (ImGui::Button("Топ мусора##mode1", ImVec2(seg_w * 0.5f, seg_h))) {
                app.view_mode = ViewMode::TopJunk;
                app.RebuildCity();
                app.ResetCamera();
            }
            ImGui::PopStyleColor(4);
        }
        ImGui::SameLine();

        // Orbit & Auto-Cam pills
        if (app.ui.orbit_mode) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.44f, 0.72f, 0.95f));
        if (ImGui::Button("Орбита (O)", ImVec2(0.0f, 30.0f))) {
            app.ui.orbit_mode = !app.ui.orbit_mode;
            app.cam.flying_to_target = false;
            if (app.ui.orbit_mode) app.ui.orbit_angle = std::atan2(app.cam.pos.z, app.cam.pos.x);
        }
        if (app.ui.orbit_mode) ImGui::PopStyleColor();
        ImGui::SameLine();

        if (app.ui.auto_cam_on_strike) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.36f, 0.56f, 0.90f));
        if (ImGui::Button(app.ui.auto_cam_on_strike ? "Авто-ракурс: ВКЛ" : "Авто-ракурс: ВЫКЛ", ImVec2(0.0f, 30.0f))) {
            app.ui.auto_cam_on_strike = !app.ui.auto_cam_on_strike;
        }
        if (app.ui.auto_cam_on_strike) ImGui::PopStyleColor();

        // Right-aligned animated freed space pill
        uint64_t shown_freed = static_cast<uint64_t>(std::llround(app.ui.displayed_freed_bytes));
        std::string freed_txt = "Снесено: " + city::FormatBytesUtf8(shown_freed) +
                                " (" + std::to_string(app.session_deleted_count) + ")";
        float tw = ImGui::CalcTextSize(freed_txt.c_str()).x + 24.0f;
        float right_x = std::max(ImGui::GetCursorPosX() + 10.0f, win_w - 28.0f - tw - 12.0f);
        ImGui::SameLine(right_x);

        ImVec2 fp = ImGui::GetCursorScreenPos();
        ImU32 pill_bg = IM_COL32(14, 42, 34, 210);
        ImU32 pill_bd = IM_COL32(55, 225, 145, static_cast<int>(130 + 125 * app.ui.freed_flash));
        wdl->AddRectFilled(fp, ImVec2(fp.x + tw, fp.y + 30.0f), pill_bg, 8.0f);
        wdl->AddRect(fp, ImVec2(fp.x + tw, fp.y + 30.0f), pill_bd, 8.0f, 0, 1.2f + app.ui.freed_flash);
        ImGui::Dummy(ImVec2(tw, 30.0f));
        wdl->AddText(ImVec2(fp.x + 12.0f, fp.y + 7.0f), IM_COL32(105, 248, 172, 255), freed_txt.c_str());
    }
    ImGui::End();

    // 2. Telemetry / Breadcrumb Bar + Category Breakdown
    city::ScanStats st = app.scanner.GetStats();
    ImGui::SetNextWindowPos(ImVec2(14.0f, 74.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(inspector_x - 24.0f, 42.0f), ImGuiCond_Always);
    if (ImGui::Begin("##Breadcrumbs", nullptr, bar_flags)) {
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        DrawGlassChrome(wdl, ImGui::GetWindowPos(), ImGui::GetWindowSize(), 12.0f);

        if (st.scanning) {
            ImVec2 wp = ImGui::GetWindowPos();
            ImVec2 ws = ImGui::GetWindowSize();
            // Animated scanning shimmer bar at bottom of breadcrumb panel
            float phase = std::fmod(time_sec * 0.85f, 1.0f);
            float bw = ws.x * 0.28f;
            float bx = wp.x + phase * (ws.x + bw) - bw;
            wdl->AddRectFilled(ImVec2(wp.x + 8.0f, wp.y + ws.y - 4.0f), ImVec2(wp.x + ws.x - 8.0f, wp.y + ws.y - 1.5f), IM_COL32(25, 40, 68, 180), 2.0f);
            wdl->AddRectFilled(ImVec2(std::max(wp.x + 8.0f, bx), wp.y + ws.y - 4.0f),
                               ImVec2(std::min(wp.x + ws.x - 8.0f, bx + bw), wp.y + ws.y - 1.5f),
                               IM_COL32(255, 200, 60, 235), 2.0f);

            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.28f, 1.0f),
                               "Сканирую: %llu файлов · %llu папок · %s (%.1fс)",
                               static_cast<unsigned long long>(st.files_scanned),
                               static_cast<unsigned long long>(st.dirs_scanned),
                               city::FormatBytesUtf8(st.bytes_scanned).c_str(),
                               st.elapsed_sec);
            ImGui::SameLine();
            if (ImGui::SmallButton("Стоп")) {
                app.scanner.CancelScan();
            }
        } else if (app.view_mode == ViewMode::TopJunk) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.28f, 1.0f),
                               "РАЙОН МУСОРА: Топ-%zu самых крупных кэшей, node_modules, Temp и логов",
                               app.buildings.size());
        } else if (app.current_dir) {
            if (app.current_dir->parent) {
                if (ImGui::SmallButton("<- Назад")) {
                    app.GoUp();
                }
                ImGui::SameLine();
            }
            std::vector<city::DirNode*> chain;
            for (city::DirNode* p = app.current_dir; p != nullptr; p = p->parent) {
                chain.push_back(p);
            }
            std::reverse(chain.begin(), chain.end());
            for (size_t i = 0; i < chain.size(); ++i) {
                if (i > 0) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(">");
                    ImGui::SameLine();
                }
                std::string lbl = chain[i]->name_utf8 + "##bc" + std::to_string(i);
                if (ImGui::SmallButton(lbl.c_str())) {
                    app.EnterDirectory(chain[i]);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", city::FormatBytesUtf8(app.current_dir->total_bytes).c_str());
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Выберите диск или папку для построения 3D-города.");
        }

        // Right-aligned Category Breakdown Stacked Bar
        if (!app.buildings.empty() && !st.scanning) {
            uint64_t totals[5]{};
            uint64_t sum = 0;
            for (const auto& b : app.buildings) {
                if (b.demolished) continue;
                int ci = static_cast<int>(b.category);
                if (b.is_protected) ci = static_cast<int>(city::NodeCategory::System);
                if (ci >= 0 && ci < 5) {
                    totals[ci] += b.bytes;
                    sum += b.bytes;
                }
            }
            if (sum > 0) {
                float bar_w = 180.0f;
                float bar_h = 10.0f;
                ImVec2 wp = ImGui::GetWindowPos();
                ImVec2 ws = ImGui::GetWindowSize();
                ImVec2 bp(wp.x + ws.x - bar_w - 14.0f, wp.y + (ws.y - bar_h) * 0.5f);
                if (ImGui::GetCursorScreenPos().x < bp.x - 16.0f) {
                    const ImU32 cols[5] = {
                        IM_COL32(62, 165, 245, 235),  // Normal
                        IM_COL32(250, 92, 54, 240),   // Junk
                        IM_COL32(190, 95, 245, 235),  // Media
                        IM_COL32(42, 215, 195, 235),  // Dev
                        IM_COL32(80, 140, 225, 235)   // System
                    };
                    wdl->AddRectFilled(bp, ImVec2(bp.x + bar_w, bp.y + bar_h), IM_COL32(15, 22, 36, 220), 5.0f);
                    float cur_x = bp.x;
                    for (int ci = 0; ci < 5; ++ci) {
                        float target_f = static_cast<float>(static_cast<double>(totals[ci]) / static_cast<double>(sum));
                        app.ui.cat_fracs[static_cast<size_t>(ci)] += (target_f - app.ui.cat_fracs[static_cast<size_t>(ci)]) * 0.12f;
                        float seg = bar_w * app.ui.cat_fracs[static_cast<size_t>(ci)];
                        if (seg > 1.0f) {
                            wdl->AddRectFilled(ImVec2(cur_x, bp.y), ImVec2(cur_x + seg, bp.y + bar_h), cols[ci], 3.0f);
                            cur_x += seg;
                        }
                    }
                    wdl->AddRect(bp, ImVec2(bp.x + bar_w, bp.y + bar_h), IM_COL32(120, 175, 235, 90), 5.0f);

                    ImVec2 mp = ImGui::GetMousePos();
                    if (mp.x >= bp.x && mp.x <= bp.x + bar_w && mp.y >= bp.y - 4.0f && mp.y <= bp.y + bar_h + 4.0f) {
                        ImGui::SetTooltip("Состав района:\nОбычные: %s\nМусор/Кэш: %s\nМедиа/Игры: %s\nDev/SDK: %s\nСистема: %s",
                                          city::FormatBytesUtf8(totals[0]).c_str(),
                                          city::FormatBytesUtf8(totals[1]).c_str(),
                                          city::FormatBytesUtf8(totals[2]).c_str(),
                                          city::FormatBytesUtf8(totals[3]).c_str(),
                                          city::FormatBytesUtf8(totals[4]).c_str());
                    }
                }
            }
        }
    }
    ImGui::End();

    // 3. Right Inspector & Explorer Panel
    ImGui::SetNextWindowPos(ImVec2(inspector_x, 74.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(inspector_w, win_h - 122.0f), ImGuiCond_Always);
    ImGuiWindowFlags insp_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##Inspector", nullptr, insp_flags)) {
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        DrawGlassChrome(wdl, ImGui::GetWindowPos(), ImGui::GetWindowSize(), 12.0f);

        if (app.ui.font_bold) ImGui::PushFont(app.ui.font_bold);
        ImGui::TextColored(ImVec4(0.45f, 0.88f, 1.0f, 1.0f), "ИНСПЕКТОР ОБЪЕКТА");
        if (app.ui.font_bold) ImGui::PopFont();
        ImGui::SameLine(inspector_w - 115.0f);
        if (ImGui::SmallButton("Сброс камеры")) {
            app.ResetCamera();
        }
        ImGui::Separator();

        if (app.selected_idx >= 0 && app.selected_idx < static_cast<int>(app.buildings.size())) {
            const auto& b = app.buildings[static_cast<size_t>(app.selected_idx)];
            city::DirNode* n = b.node;
            if (n) {
                ImVec2 card_pos = ImGui::GetCursorScreenPos();
                float card_w = inspector_w - 28.0f;
                ImVec4 cvec = CategoryVec4(n->category, n->is_protected);
                ImU32 ccol = CategoryBadgeColor(n->category, n->is_protected);

                if (ImGui::BeginChild("##SelCard", ImVec2(card_w, 242.0f), true, ImGuiWindowFlags_NoScrollbar)) {
                    ImDrawList* cdl = ImGui::GetWindowDrawList();
                    ImVec2 cp = ImGui::GetWindowPos();
                    ImVec2 cs = ImGui::GetWindowSize();
                    // Left category accent stripe
                    cdl->AddRectFilled(ImVec2(cp.x, cp.y + 4.0f), ImVec2(cp.x + 4.0f, cp.y + cs.y - 4.0f), ccol, 2.0f);

                    if (app.ui.font_bold) ImGui::PushFont(app.ui.font_bold);
                    ImGui::TextWrapped("%s", n->name_utf8.c_str());
                    if (app.ui.font_bold) ImGui::PopFont();

                    // Category badge pill + size
                    ImGui::TextColored(cvec, "%s", CategoryNameRu(n->category, n->is_protected));
                    ImGui::SameLine();
                    if (app.ui.font_bold) ImGui::PushFont(app.ui.font_bold);
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.30f, 1.0f), "· %s", city::FormatBytesUtf8(n->total_bytes).c_str());
                    if (app.ui.font_bold) ImGui::PopFont();

                    // Parent share progress bar
                    uint64_t ref_bytes = (app.current_dir && app.current_dir->total_bytes > 0) ? app.current_dir->total_bytes : n->total_bytes;
                    float share = (ref_bytes > 0) ? std::clamp(static_cast<float>(static_cast<double>(n->total_bytes) / static_cast<double>(ref_bytes)), 0.0f, 1.0f) : 0.0f;
                    char pct_buf[32]{};
                    snprintf(pct_buf, sizeof(pct_buf), "%.1f%% от текущей папки", share * 100.0f);
                    ImGui::ProgressBar(share * app.ui.inspector_reveal, ImVec2(-1.0f, 14.0f), pct_buf);

                    ImGui::Text("Файлов: %llu   ·   Папок: %llu   ·   Высота: %.0fм",
                                static_cast<unsigned long long>(n->file_count),
                                static_cast<unsigned long long>(n->dir_count),
                                b.target_height);

                    ImGui::TextDisabled("%s", n->path_utf8.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", n->path_utf8.c_str());

                    ImGui::Spacing();
                    if (n->is_dir && !n->deleted && app.root_tree) {
                        if (ImGui::Button("Войти в папку", ImVec2(158.0f, 30.0f))) {
                            app.EnterDirectory(n);
                        }
                        ImGui::SameLine();
                    }
                    if (ImGui::Button("Проводник", ImVec2(n->is_dir ? 158.0f : -1.0f, 30.0f))) {
                        std::wstring arg = L"/select,\"" + n->full_path + L"\"";
                        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
                    }

                    if (n->is_protected) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.22f, 0.34f, 0.85f));
                        ImGui::Button("СИСТЕМНАЯ ЗАЩИТА (нельзя снести)", ImVec2(-1.0f, 34.0f));
                        ImGui::PopStyleColor();
                    } else if (!app.root_tree) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.22f, 0.34f, 0.85f));
                        ImGui::Button("Дождитесь окончания сканирования...", ImVec2(-1.0f, 34.0f));
                        ImGui::PopStyleColor();
                    } else if (b.strike_incoming || b.collapsing) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.32f, 0.12f, 0.90f));
                        ImGui::Button("ЛИКВИДАЦИЯ В ПРОЦЕССЕ...", ImVec2(-1.0f, 34.0f));
                        ImGui::PopStyleColor();
                    } else if (!n->deleted && !b.demolished) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.76f, 0.16f, 0.20f, 0.92f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.94f, 0.24f, 0.26f, 1.00f));
                        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.00f, 0.45f, 0.38f, 0.85f));
                        if (ImGui::Button("СНЕСТИ В КОРЗИНУ (Del)", ImVec2(-1.0f, 34.0f))) {
                            app.pending_delete_node = n;
                            app.pending_delete_building_idx = app.selected_idx;
                            app.confirm_delete_open = true;
                        }
                        ImGui::PopStyleColor(3);
                    }
                }
                ImGui::EndChild();
                (void)card_pos;
            }
        } else {
            ImGui::BeginChild("##EmptySel", ImVec2(-1.0f, 76.0f), true);
            ImGui::TextDisabled("Выберите небоскрёб кликом ЛКМ в 3D-городе\nили из списка ниже для анализа и сноса.");
            ImGui::EndChild();
        }

        ImGui::Spacing();
        if (app.ui.font_bold) ImGui::PushFont(app.ui.font_bold);
        ImGui::TextColored(ImVec4(0.45f, 0.88f, 1.0f, 1.0f), "ТОП ОБЪЕКТОВ (%zu)", app.buildings.size());
        if (app.ui.font_bold) ImGui::PopFont();

        // Search + Quick Category Filter Pills
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##SearchObj", "Фильтр по названию...", app.ui.search_buf, sizeof(app.ui.search_buf));

        auto filter_btn = [&](const char* lbl, int cat_val) {
            bool act = (app.ui.filter_cat == cat_val);
            if (act) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.44f, 0.72f, 0.98f));
            if (ImGui::SmallButton(lbl)) app.ui.filter_cat = cat_val;
            if (act) ImGui::PopStyleColor();
        };
        filter_btn("Все", -1); ImGui::SameLine();
        filter_btn("Мусор", static_cast<int>(city::NodeCategory::Junk)); ImGui::SameLine();
        filter_btn("Медиа", static_cast<int>(city::NodeCategory::Media)); ImGui::SameLine();
        filter_btn("Dev", static_cast<int>(city::NodeCategory::Dev)); ImGui::SameLine();
        filter_btn("Обычные", static_cast<int>(city::NodeCategory::Normal));

        std::string query = ToLowerAscii(app.ui.search_buf);
        uint64_t top_bytes = app.buildings.empty() ? 1 : std::max<uint64_t>(1, app.buildings.front().bytes);

        if (ImGui::BeginChild("##TopList", ImVec2(0.0f, 0.0f), true)) {
            ImDrawList* ldl = ImGui::GetWindowDrawList();
            for (int i = 0; i < static_cast<int>(app.buildings.size()); ++i) {
                const auto& b = app.buildings[static_cast<size_t>(i)];
                if (b.collapsing || b.demolished || b.strike_incoming) continue;
                if (app.ui.filter_cat >= 0 && static_cast<int>(b.category) != app.ui.filter_cat) continue;
                if (!query.empty() && ToLowerAscii(b.label).find(query) == std::string::npos) continue;

                bool is_sel = (i == app.selected_idx);
                ImVec2 row_p = ImGui::GetCursorScreenPos();
                float row_w = ImGui::GetContentRegionAvail().x;
                float row_h = 26.0f;

                float rel = std::clamp(static_cast<float>(static_cast<double>(b.bytes) / static_cast<double>(top_bytes)), 0.02f, 1.0f);
                ImU32 bar_bg = is_sel ? IM_COL32(60, 140, 225, 95) : IM_COL32(35, 78, 130, 52);
                ldl->AddRectFilled(row_p, ImVec2(row_p.x + row_w * rel, row_p.y + row_h), bar_bg, 5.0f);

                ImU32 dot_c = CategoryBadgeColor(b.category, b.is_protected);
                ldl->AddCircleFilled(ImVec2(row_p.x + 9.0f, row_p.y + row_h * 0.5f), 3.6f, dot_c);

                ImVec4 tcol = b.is_protected ? ImVec4(0.62f, 0.78f, 0.98f, 1.0f)
                            : b.is_junk      ? ImVec4(1.00f, 0.52f, 0.36f, 1.0f)
                                             : ImVec4(0.93f, 0.96f, 1.00f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, tcol);
                std::string row_lbl = "    " + b.label + "##row" + std::to_string(i);
                if (ImGui::Selectable(row_lbl.c_str(), is_sel, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, row_h))) {
                    app.selected_idx = i;
                    app.FocusOnBuilding(b);
                    if (ImGui::IsMouseDoubleClicked(0) && b.node && b.node->is_dir) {
                        app.EnterDirectory(b.node);
                    }
                }
                ImGui::PopStyleColor();

                // Right-aligned size readout on the same row
                ImVec2 sz_t = ImGui::CalcTextSize(b.size_label.c_str());
                ldl->AddText(ImVec2(row_p.x + row_w - sz_t.x - 8.0f, row_p.y + 4.0f),
                             IM_COL32(195, 215, 240, 235), b.size_label.c_str());
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();

    // 4. Bottom Controls Hint Bar
    ImGui::SetNextWindowPos(ImVec2(14.0f, win_h - 38.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(win_w - 28.0f, 28.0f), ImGuiCond_Always);
    if (ImGui::Begin("##BottomBar", nullptr, bar_flags)) {
        ImGui::TextDisabled("WASD / Q,E (+Shift) — полёт · ПКМ — обзор · Колесо — зум · ЛКМ — выбрать · 2xЛКМ — войти · Backspace — назад · Del — снести · O — орбита · R — сброс");
    }
    ImGui::End();

    // 5. Floating Animated Toast Notification Pill (above bottom bar)
    if (app.ui.toast_anim > 0.01f && !app.toast_msg.empty()) {
        ImDrawList* fdl = ImGui::GetForegroundDrawList();
        ImVec2 tsz = ImGui::CalcTextSize(app.toast_msg.c_str());
        float tw = tsz.x + 36.0f;
        float th = 36.0f;
        float tx = (inspector_x - tw) * 0.5f;
        float ty = win_h - 46.0f - th - 14.0f * app.ui.toast_anim;
        int alpha = static_cast<int>(240.0f * app.ui.toast_anim);

        ImU32 bg = app.toast_is_error ? IM_COL32(48, 14, 18, alpha) : IM_COL32(12, 34, 28, alpha);
        ImU32 bd = app.toast_is_error ? IM_COL32(255, 85, 85, alpha) : IM_COL32(65, 235, 155, alpha);
        ImU32 tx_col = app.toast_is_error ? IM_COL32(255, 185, 185, alpha) : IM_COL32(165, 255, 210, alpha);

        fdl->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + tw, ty + th), bg, 10.0f);
        fdl->AddRect(ImVec2(tx, ty), ImVec2(tx + tw, ty + th), bd, 10.0f, 0, 1.5f);
        fdl->AddText(ImVec2(tx + 18.0f, ty + (th - tsz.y) * 0.5f - 1.0f), tx_col, app.toast_msg.c_str());

        float prog = std::clamp(app.toast_timer / std::max(0.1f, app.toast_max_timer), 0.0f, 1.0f);
        fdl->AddRectFilled(ImVec2(tx + 10.0f, ty + th - 4.0f),
                           ImVec2(tx + 10.0f + (tw - 20.0f) * prog, ty + th - 2.0f), bd, 1.0f);
    }

    // 6. Confirm Recycle Bin Modal
    if (app.confirm_delete_open) {
        ImGui::OpenPopup("Подтверждение сноса");
        app.confirm_delete_open = false;
    }
    if (ImGui::BeginPopupModal("Подтверждение сноса", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (app.pending_delete_node) {
            if (app.ui.font_bold) ImGui::PushFont(app.ui.font_bold);
            ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.30f, 1.0f), "Отправить этот объект в Корзину Windows?");
            if (app.ui.font_bold) ImGui::PopFont();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.30f, 1.0f), "%s (%s)",
                               app.pending_delete_node->name_utf8.c_str(),
                               city::FormatBytesUtf8(app.pending_delete_node->total_bytes).c_str());
            ImGui::TextDisabled("Файлов: %llu", static_cast<unsigned long long>(app.pending_delete_node->file_count));
            ImGui::TextWrapped("%s", app.pending_delete_node->path_utf8.c_str());
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.84f, 0.20f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.96f, 0.28f, 0.28f, 1.0f));
            if (ImGui::Button("Да, снести в Корзину!", ImVec2(185.0f, 34.0f))) {
                uint64_t freed = 0;
                std::string err;
                int target_idx = app.FindBuildingIndex(app.pending_delete_node, app.pending_delete_building_idx);
                if (city::DiskScanner::MoveToRecycleBin(app.pending_delete_node, freed, err)) {
                    app.session_freed_bytes += freed;
                    app.session_deleted_count++;
                    app.ui.freed_flash = 1.0f;
                    app.SpawnPlaneStrike(target_idx);
                    app.selected_idx = -1;
                    app.ShowToast("Снесено в Корзину: " + app.pending_delete_node->name_utf8 +
                                  " (-" + city::FormatBytesUtf8(freed) + ")");
                } else {
                    app.ShowToast(err, true);
                }
                app.pending_delete_node = nullptr;
                app.pending_delete_building_idx = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (ImGui::Button("Отмена", ImVec2(120.0f, 34.0f))) {
                app.pending_delete_node = nullptr;
                app.pending_delete_building_idx = -1;
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SsdCityWindowClass";
    if (!RegisterClassExW(&wc)) {
        CoUninitialize();
        return 1;
    }

    AppState app;
    g_app = &app;
    app.InitTraffic();

    int init_w = 1480;
    int init_h = 880;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    init_w = std::min(init_w, static_cast<int>(work.right - work.left - 32));
    init_h = std::min(init_h, static_cast<int>(work.bottom - work.top - 32));
    int wx = work.left + ((work.right - work.left) - init_w) / 2;
    int wy = work.top + ((work.bottom - work.top) - init_h) / 2;

    app.hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"SSD City — 3D-город из папок",
        WS_OVERLAPPEDWINDOW,
        wx, wy, init_w, init_h,
        nullptr, nullptr, hInstance, nullptr
    );
    if (!app.hwnd) {
        CoUninitialize();
        return 1;
    }

    RECT cr{};
    GetClientRect(app.hwnd, &cr);
    if (!app.renderer.Init(app.hwnd, cr.right - cr.left, cr.bottom - cr.top)) {
        DestroyWindow(app.hwnd);
        CoUninitialize();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ApplyPremiumTheme();

    UINT dpi = GetDpiForWindow(app.hwnd);
    float dpi_scale = static_cast<float>(dpi ? dpi : 96) / 96.0f;
    float font_px = 15.5f * dpi_scale;
    float bold_px = 16.5f * dpi_scale;

    static ImVector<ImWchar> glyph_ranges;
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    builder.AddText("\xE2\x80\x94\xC2\xB7");
    builder.BuildRanges(&glyph_ranges);

    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES) {
        app.ui.font_regular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", font_px, nullptr, glyph_ranges.Data);
    } else if (GetFileAttributesW(L"C:\\Windows\\Fonts\\arial.ttf") != INVALID_FILE_ATTRIBUTES) {
        app.ui.font_regular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", font_px, nullptr, glyph_ranges.Data);
    } else {
        app.ui.font_regular = io.Fonts->AddFontDefault();
    }

    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\seguisb.ttf") != INVALID_FILE_ATTRIBUTES) {
        app.ui.font_bold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", bold_px, nullptr, glyph_ranges.Data);
    } else if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeuib.ttf") != INVALID_FILE_ATTRIBUTES) {
        app.ui.font_bold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", bold_px, nullptr, glyph_ranges.Data);
    } else {
        app.ui.font_bold = app.ui.font_regular;
    }

    ImGui_ImplWin32_Init(app.hwnd);
    ImGui_ImplDX11_Init(app.renderer.GetDevice(), app.renderer.GetContext());

    app.drives = city::DiskScanner::EnumerateDrives();
    app.StartScanPath(GetUserProfileDir());

    ShowWindow(app.hwnd, SW_SHOWDEFAULT);
    UpdateWindow(app.hwnd);

    auto t_start = std::chrono::steady_clock::now();
    auto t_prev = t_start;
    float live_refresh_accum = 0.0f;

    MSG msg{};
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        auto t_now = std::chrono::steady_clock::now();
        float dt = std::clamp(std::chrono::duration<float>(t_now - t_prev).count(), 0.001f, 0.05f);
        float time_sec = std::chrono::duration<float>(t_now - t_start).count();
        t_prev = t_now;

        if (auto completed = app.scanner.TakeCompletedTree()) {
            if (!app.root_tree) {
                app.root_tree = std::move(completed);
                app.current_dir = app.root_tree.get();
                app.RebuildCity();
                app.ResetCamera();
            }
        } else if (!app.root_tree) {
            live_refresh_accum += dt;
            if (live_refresh_accum >= 0.25f) {
                live_refresh_accum = 0.0f;
                auto live_nodes = app.scanner.GetLiveTopNodes();
                if (!live_nodes.empty()) {
                    city::DirNode* sel_node = (app.selected_idx >= 0 && app.selected_idx < static_cast<int>(app.buildings.size()))
                        ? app.buildings[static_cast<size_t>(app.selected_idx)].node : nullptr;
                    auto next_b = city::CityLayout::BuildFromWeightedNodes(live_nodes, 96.0f);
                    int new_sel = -1;
                    for (int i = 0; i < static_cast<int>(next_b.size()); ++i) {
                        auto& nb = next_b[static_cast<size_t>(i)];
                        if (sel_node && nb.node == sel_node) new_sel = i;
                        bool found = false;
                        for (const auto& ob : app.buildings) {
                            if (ob.node == nb.node) {
                                nb.height = ob.height;
                                nb.spawn_progress = 1.0f;
                                nb.spawn_delay = 0.0f;
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            nb.height = 0.2f;
                            nb.spawn_progress = 0.0f;
                            nb.spawn_delay = 0.0f;
                        }
                    }
                    app.buildings = std::move(next_b);
                    app.props = city::CityLayout::GenerateProps(app.buildings);
                    app.selected_idx = new_sel;
                    app.hovered_idx = -1;
                    app.static_mesh_epoch++;
                }
            }
        }

        UpdateCameraAndPhysics(app, dt, time_sec);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawUI(app, time_sec);

        ImGui::Render();
        app.renderer.BeginFrame(time_sec);
        app.renderer.RenderCity(app.buildings, app.props, app.strikes, app.cars, app.particles,
                                app.hovered_idx, app.selected_idx, app.cam, time_sec, app.static_mesh_epoch);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        app.renderer.EndFrame();
    }

    app.scanner.CancelScan();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    app.renderer.Shutdown();
    UnregisterClassW(wc.lpszClassName, hInstance);
    CoUninitialize();
    return 0;
}
