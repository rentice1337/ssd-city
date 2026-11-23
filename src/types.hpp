#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace city {

enum class NodeCategory {
    Normal = 0,
    Junk,
    Media,
    Dev,
    System
};

struct DirNode {
    std::wstring name;
    std::wstring full_path;
    std::string name_utf8;
    std::string path_utf8;

    bool is_dir = true;
    bool is_protected = false;
    bool is_junk = false;
    bool deleted = false;
    NodeCategory category = NodeCategory::Normal;

    uint64_t own_file_bytes = 0;
    uint64_t total_bytes = 0;
    std::atomic<uint64_t> live_bytes{0};
    uint64_t file_count = 0;
    uint64_t dir_count = 0;

    DirNode* parent = nullptr;
    std::vector<std::unique_ptr<DirNode>> children;
};

struct Building {
    DirNode* node = nullptr;
    std::string label;
    std::string size_label;
    NodeCategory category = NodeCategory::Normal;
    bool is_protected = false;
    bool is_junk = false;

    float x0 = 0.0f;
    float z0 = 0.0f;
    float x1 = 0.0f;
    float z1 = 0.0f;
    float height = 1.0f;
    float target_height = 1.0f;

    // Spawn rise animation
    float spawn_delay = 0.0f;
    float spawn_timer = 0.0f;
    float spawn_progress = 1.0f;

    // Architectural variation seed
    uint32_t style_seed = 0;

    // Plane strike & progressive collapse state
    bool strike_incoming = false;
    bool collapsing = false;
    bool demolished = false;
    float collapse_timer = 0.0f;
    float shake_offset_x = 0.0f;
    float shake_offset_z = 0.0f;
    float impact_y = 0.0f;
    float impact_dir_x = 1.0f;
    float impact_dir_z = 0.0f;
    float burn_intensity = 0.0f;
    float upper_tilt = 0.0f;
    float upper_drop = 0.0f;
    float lower_scale = 1.0f;

    uint32_t id = 0;
    uint64_t bytes = 0;

    bool IsStaticSettled() const {
        return !collapsing && !demolished && spawn_progress >= 1.0f;
    }
};

struct DriveInfo {
    std::wstring root_path;
    std::string label_utf8;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t used_bytes = 0;
};

struct ScanStats {
    bool scanning = false;
    bool completed = false;
    bool cancelled = false;
    uint64_t files_scanned = 0;
    uint64_t dirs_scanned = 0;
    uint64_t bytes_scanned = 0;
    double elapsed_sec = 0.0;
    std::string current_path_utf8;
};

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

inline std::string FormatBytesUtf8(uint64_t bytes) {
    char buf[64]{};
    const double tb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0 * 1024.0);
    if (tb >= 1.0) {
        snprintf(buf, sizeof(buf), "%.2f TB", tb);
        return buf;
    }
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0) {
        snprintf(buf, sizeof(buf), "%.2f GB", gb);
        return buf;
    }
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 1.0) {
        snprintf(buf, sizeof(buf), "%.1f MB", mb);
        return buf;
    }
    const double kb = static_cast<double>(bytes) / 1024.0;
    if (kb >= 1.0) {
        snprintf(buf, sizeof(buf), "%.0f KB", kb);
        return buf;
    }
    snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}

} // namespace city
