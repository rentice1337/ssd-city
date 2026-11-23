#pragma once

#include "city_layout.hpp"
#include "types.hpp"
#include <windows.h>
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>

namespace city {

struct Camera {
    DirectX::XMFLOAT3 pos{-54.0f, 68.0f, -98.0f};
    DirectX::XMFLOAT3 vel{0.0f, 0.0f, 0.0f};
    float yaw = 0.48f;    // radians, 0 = looking toward +Z
    float pitch = 0.48f;  // radians, positive = looking down

    bool flying_to_target = false;
    DirectX::XMFLOAT3 target_pos{-54.0f, 68.0f, -98.0f};
    float target_yaw = 0.48f;
    float target_pitch = 0.48f;

    float shake_trauma = 0.0f;
    DirectX::XMFLOAT3 shake_offset{0.0f, 0.0f, 0.0f};

    DirectX::XMMATRIX GetViewMatrix() const;
    DirectX::XMFLOAT3 GetForward() const;
    DirectX::XMFLOAT3 GetRight() const;
};

struct Particle3D {
    enum class Kind {
        Debris = 0,
        Fire,
        Smoke,
        Spark,
        Shockwave
    };
    Kind kind = Kind::Debris;
    DirectX::XMFLOAT3 pos{};
    DirectX::XMFLOAT3 vel{};
    DirectX::XMFLOAT3 rot{};
    DirectX::XMFLOAT3 rot_vel{};
    DirectX::XMFLOAT4 color{};
    float size = 0.6f;
    float end_size = 0.6f;
    float life = 1.5f;
    float max_life = 1.5f;
};

struct PlaneStrike {
    int building_idx = -1;
    DirNode* target_node = nullptr;
    uint32_t building_id = 0;

    DirectX::XMFLOAT3 start_pos{};
    DirectX::XMFLOAT3 impact_pos{};
    DirectX::XMFLOAT3 pos{};
    DirectX::XMFLOAT3 dir{};

    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    float progress = 0.0f;
    float duration = 1.45f;
    float post_impact_timer = 0.0f;
    float trail_accum = 0.0f;
    float scale = 1.0f;

    bool impacted = false;
    bool finished = false;
};

struct TrafficCar {
    float axis_pos = 0.0f;
    float lane_coord = 0.0f;
    float speed = 14.0f;
    bool along_z = true;
    int dir = 1;
    DirectX::XMFLOAT4 color{};
};

class CityRenderer {
public:
    CityRenderer() = default;
    ~CityRenderer();

    bool Init(HWND hwnd, int width, int height);
    void Resize(int width, int height);
    void Shutdown();

    void BeginFrame(float time_sec);
    void RenderCity(const std::vector<Building>& buildings,
                    const std::vector<CityProp>& props,
                    const std::vector<PlaneStrike>& strikes,
                    const std::vector<TrafficCar>& cars,
                    const std::vector<Particle3D>& particles,
                    int hovered_idx,
                    int selected_idx,
                    const Camera& cam,
                    float time_sec,
                    uint64_t static_mesh_epoch);
    void EndFrame();

    int PickBuilding(const std::vector<Building>& buildings, const Camera& cam, float mouse_x, float mouse_y) const;
    bool ProjectToScreen(const DirectX::XMFLOAT3& world_pos, const Camera& cam, float& out_x, float& out_y) const;

    ID3D11Device* GetDevice() const { return device_.Get(); }
    ID3D11DeviceContext* GetContext() const { return ctx_.Get(); }
    int Width() const { return width_; }
    int Height() const { return height_; }

private:
    bool CreateRenderTargets();
    bool CreateShadersAndStates();
    void RebuildStaticMesh(const std::vector<Building>& buildings, const std::vector<CityProp>& props);
    void UpdateDynamicMeshes(const std::vector<Building>& buildings,
                             const std::vector<PlaneStrike>& strikes,
                             const std::vector<TrafficCar>& cars,
                             const std::vector<Particle3D>& particles,
                             int hovered_idx,
                             int selected_idx,
                             float time_sec);

    int width_ = 1280;
    int height_ = 720;
    UINT msaa_count_ = 1;
    UINT msaa_quality_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_tex_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> sky_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> sky_ps_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> sky_layout_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> bld_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> bld_ps_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> bld_layout_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> gnd_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> gnd_ps_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> gnd_layout_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> pt_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pt_ps_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> pt_layout_;

    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_frame_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> sky_vb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> sky_ib_;
    UINT sky_index_count_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Buffer> gnd_vb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> gnd_ib_;

    // Cached static city geometry (settled buildings + props)
    uint64_t cached_epoch_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> static_vb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> static_ib_;
    UINT static_vb_cap_ = 0;
    UINT static_ib_cap_ = 0;
    UINT static_index_count_ = 0;

    // Per-frame dynamic opaque geometry (animating buildings, planes, cars, solid debris)
    Microsoft::WRL::ComPtr<ID3D11Buffer> dyn_vb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> dyn_ib_;
    UINT dyn_vb_cap_ = 0;
    UINT dyn_ib_cap_ = 0;
    UINT dyn_index_count_ = 0;

    // Per-frame alpha-blended geometry (smoke, shockwaves, selection rings)
    Microsoft::WRL::ComPtr<ID3D11Buffer> alpha_vb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> alpha_ib_;
    UINT alpha_vb_cap_ = 0;
    UINT alpha_ib_cap_ = 0;
    UINT alpha_index_count_ = 0;

    // Per-frame additive geometry (fireballs, sparks, impact flash, halos)
    Microsoft::WRL::ComPtr<ID3D11Buffer> add_vb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> add_ib_;
    UINT add_vb_cap_ = 0;
    UINT add_ib_cap_ = 0;
    UINT add_index_count_ = 0;

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rs_solid_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rs_nocull_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> dss_default_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> dss_sky_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> dss_readonly_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> bs_opaque_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> bs_alpha_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> bs_additive_;
};

} // namespace city
