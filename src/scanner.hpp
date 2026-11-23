#pragma once

#include "types.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace city {

class DiskScanner {
public:
    DiskScanner() = default;
    ~DiskScanner();

    DiskScanner(const DiskScanner&) = delete;
    DiskScanner& operator=(const DiskScanner&) = delete;

    static std::vector<DriveInfo> EnumerateDrives();

    void StartScan(const std::wstring& root_path);
    void CancelScan();

    ScanStats GetStats() const;
    std::shared_ptr<DirNode> TakeCompletedTree();
    std::vector<std::pair<DirNode*, uint64_t>> GetLiveTopNodes() const;

    static std::vector<DirNode*> CollectTopJunk(DirNode* root, size_t max_items = 100);
    static bool MoveToRecycleBin(DirNode* node, uint64_t& out_freed_bytes, std::string& out_error);

private:
    void CoordinatorThread(std::wstring root_path);

    std::atomic<bool> cancel_{false};
    std::atomic<bool> scanning_{false};
    std::atomic<bool> completed_{false};
    std::atomic<uint64_t> files_scanned_{0};
    std::atomic<uint64_t> dirs_scanned_{0};
    std::atomic<uint64_t> bytes_scanned_{0};

    mutable std::mutex path_mtx_;
    std::string current_path_utf8_;
    double elapsed_sec_ = 0.0;

    mutable std::mutex tree_mtx_;
    std::shared_ptr<DirNode> live_root_;
    std::shared_ptr<DirNode> completed_tree_;

    std::thread coord_thread_;
};

} // namespace city
