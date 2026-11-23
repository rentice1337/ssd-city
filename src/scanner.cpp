#include "scanner.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <unordered_set>

namespace city {

namespace {

std::wstring ToLower(std::wstring s) {
    for (auto& ch : s) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return s;
}

std::wstring NormalizeDirPath(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) {
        p.pop_back();
    }
    if (p.size() == 2 && p[1] == L':') {
        p.push_back(L'\\');
    }
    return p;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

std::wstring GetExtensionLower(const std::wstring& name) {
    size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos) return {};
    return ToLower(name.substr(dot));
}

void ClassifyNode(DirNode& node, int depth) {
    std::wstring lower_name = ToLower(node.name);
    std::wstring lower_path = ToLower(node.full_path);

    // Root drive or top-level critical Windows folders
    if (depth == 0) {
        node.is_protected = true;
        node.category = NodeCategory::System;
        return;
    }

    static const std::unordered_set<std::wstring> kProtectedNames = {
        L"windows", L"program files", L"program files (x86)", L"programdata",
        L"$recycle.bin", L"system volume information", L"recovery", L"boot",
        L"perflogs", L"users", L"appdata", L"local", L"roaming", L"locallow",
        L"pagefile.sys", L"hiberfil.sys", L"swapfile.sys", L"dumpstack.log.tmp",
        L"bootmgr", L"bootnxt"
    };

    if (kProtectedNames.find(lower_name) != kProtectedNames.end()) {
        // Protect top-level system folders and AppData containers themselves
        if (depth <= 2 || lower_name == L"appdata" || lower_name == L"local" ||
            lower_name == L"roaming" || lower_name == L"locallow" ||
            lower_name == L"pagefile.sys" || lower_name == L"hiberfil.sys" ||
            lower_name == L"swapfile.sys") {
            node.is_protected = true;
            node.category = NodeCategory::System;
            return;
        }
    }

    // Anything directly inside C:\Windows is protected
    if (lower_path.find(L":\\windows\\") != std::wstring::npos ||
        lower_path.ends_with(L":\\windows")) {
        node.is_protected = true;
        node.category = NodeCategory::System;
        return;
    }

    static const std::unordered_set<std::wstring> kJunkDirs = {
        L"node_modules", L".gradle", L".cargo", L"__pycache__", L".venv", L"venv",
        L"cache", L"code cache", L"gpucache", L"shadercache", L"dxcache", L"glcache",
        L"temp", L"tmp", L"crashdumps", L"logs", L".nuget", L"npm-cache", L"_cacache",
        L"pip", L".next", L".nuxt", L"deriveddata", L"webcache", L"service worker",
        L"cmake-build-debug", L"cmake-build-release", L"intermediate", L"cachestorage",
        L"dawncache", L"grshadercache", L"nvidia", L"dx_cache"
    };

    static const std::unordered_set<std::wstring> kJunkExts = {
        L".tmp", L".log", L".dmp", L".bak", L".old", L".etl"
    };

    static const std::unordered_set<std::wstring> kMediaDirs = {
        L"steamlibrary", L"steamapps", L"epic games", L"riot games", L"ubisoft",
        L"downloads", L"videos", L"movies", L"music", L"pictures", L"загрузки", L"видео"
    };

    static const std::unordered_set<std::wstring> kMediaExts = {
        L".mp4", L".mkv", L".avi", L".mov", L".wmv", L".iso", L".zip", L".rar", L".7z", L".tar", L".gz"
    };

    static const std::unordered_set<std::wstring> kDevDirs = {
        L".git", L".vs", L"visual studio", L"android", L"sdk", L"docker", L"wsl",
        L"miniconda3", L"anaconda3", L"rustup", L"go", L"build", L"dist", L"out", L"target"
    };

    if (node.is_dir) {
        if (kJunkDirs.find(lower_name) != kJunkDirs.end() ||
            lower_name.find(L"cache") != std::wstring::npos ||
            lower_name.find(L"temp") != std::wstring::npos) {
            node.is_junk = true;
            node.category = NodeCategory::Junk;
            return;
        }
        if (kMediaDirs.find(lower_name) != kMediaDirs.end()) {
            node.category = NodeCategory::Media;
            return;
        }
        if (kDevDirs.find(lower_name) != kDevDirs.end()) {
            node.category = NodeCategory::Dev;
            return;
        }
    } else {
        std::wstring ext = GetExtensionLower(node.name);
        if (kJunkExts.find(ext) != kJunkExts.end()) {
            node.is_junk = true;
            node.category = NodeCategory::Junk;
            return;
        }
        if (kMediaExts.find(ext) != kMediaExts.end()) {
            node.category = NodeCategory::Media;
            return;
        }
    }

    node.category = NodeCategory::Normal;
}

void AggregateTree(DirNode* node, DirNode* parent) {
    node->parent = parent;
    if (!node->is_dir) {
        return;
    }

    uint64_t sum_bytes = node->own_file_bytes;
    uint64_t sum_files = node->file_count;
    uint64_t sum_dirs = 0;

    for (auto& ch : node->children) {
        AggregateTree(ch.get(), node);
        sum_bytes += ch->total_bytes;
        sum_files += ch->file_count;
        if (ch->is_dir) {
            sum_dirs += 1 + ch->dir_count;
        }
    }

    node->total_bytes = sum_bytes;
    node->file_count = sum_files;
    node->dir_count = sum_dirs;

    std::sort(node->children.begin(), node->children.end(),
              [](const std::unique_ptr<DirNode>& a, const std::unique_ptr<DirNode>& b) {
                  return a->total_bytes > b->total_bytes;
              });
}

} // namespace

DiskScanner::~DiskScanner() {
    CancelScan();
}

std::vector<DriveInfo> DiskScanner::EnumerateDrives() {
    std::vector<DriveInfo> drives;
    wchar_t buf[512]{};
    DWORD len = GetLogicalDriveStringsW(511, buf);
    if (len == 0) return drives;

    const wchar_t* p = buf;
    while (*p) {
        UINT type = GetDriveTypeW(p);
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
            DriveInfo di{};
            di.root_path = p;

            ULARGE_INTEGER free_avail{}, total{}, total_free{};
            if (GetDiskFreeSpaceExW(p, &free_avail, &total, &total_free)) {
                di.total_bytes = total.QuadPart;
                di.free_bytes = total_free.QuadPart;
                di.used_bytes = (total.QuadPart >= total_free.QuadPart) ? (total.QuadPart - total_free.QuadPart) : 0;
            }

            wchar_t vol_name[128]{};
            GetVolumeInformationW(p, vol_name, 127, nullptr, nullptr, nullptr, nullptr, 0);
            std::string root_u8 = WideToUtf8(p);
            if (!root_u8.empty() && root_u8.back() == '\\') root_u8.pop_back();
            std::string vol_u8 = WideToUtf8(vol_name);
            if (!vol_u8.empty()) {
                di.label_utf8 = root_u8 + " [" + vol_u8 + "]";
            } else {
                di.label_utf8 = root_u8;
            }
            drives.push_back(std::move(di));
        }
        p += wcslen(p) + 1;
    }
    return drives;
}

void DiskScanner::StartScan(const std::wstring& root_path) {
    CancelScan();
    cancel_.store(false);
    completed_.store(false);
    scanning_.store(true);
    files_scanned_.store(0);
    dirs_scanned_.store(0);
    bytes_scanned_.store(0);
    elapsed_sec_ = 0.0;

    {
        std::lock_guard<std::mutex> lock(tree_mtx_);
        live_root_.reset();
        completed_tree_.reset();
    }

    coord_thread_ = std::thread(&DiskScanner::CoordinatorThread, this, NormalizeDirPath(root_path));
}

void DiskScanner::CancelScan() {
    cancel_.store(true);
    if (coord_thread_.joinable()) {
        coord_thread_.join();
    }
    scanning_.store(false);
}

ScanStats DiskScanner::GetStats() const {
    ScanStats st{};
    st.scanning = scanning_.load();
    st.completed = completed_.load();
    st.cancelled = cancel_.load();
    st.files_scanned = files_scanned_.load();
    st.dirs_scanned = dirs_scanned_.load();
    st.bytes_scanned = bytes_scanned_.load();
    {
        std::lock_guard<std::mutex> lock(path_mtx_);
        st.current_path_utf8 = current_path_utf8_;
        st.elapsed_sec = elapsed_sec_;
    }
    return st;
}

std::shared_ptr<DirNode> DiskScanner::TakeCompletedTree() {
    std::lock_guard<std::mutex> lock(tree_mtx_);
    return completed_tree_;
}

std::vector<std::pair<DirNode*, uint64_t>> DiskScanner::GetLiveTopNodes() const {
    std::vector<std::pair<DirNode*, uint64_t>> out;
    std::shared_ptr<DirNode> lr;
    {
        std::lock_guard<std::mutex> lock(tree_mtx_);
        lr = live_root_;
    }
    if (!lr) return out;
    out.reserve(lr->children.size());
    for (const auto& ch : lr->children) {
        uint64_t b = ch->live_bytes.load(std::memory_order_relaxed);
        if (b > 0) {
            out.push_back({ch.get(), b});
        }
    }
    return out;
}

void DiskScanner::CoordinatorThread(std::wstring root_path) {
    auto t0 = std::chrono::steady_clock::now();

    auto root = std::make_shared<DirNode>();
    root->full_path = root_path;
    root->name = root_path;
    if (root->name.size() > 3) {
        size_t slash = root->name.rfind(L'\\');
        if (slash != std::wstring::npos) {
            root->name = root->name.substr(slash + 1);
        }
    }
    root->name_utf8 = WideToUtf8(root->name);
    root->path_utf8 = WideToUtf8(root->full_path);
    root->is_dir = true;
    ClassifyNode(*root, 0);

    struct Task {
        DirNode* node;
        int depth;
        DirNode* top_bucket;
    };

    std::mutex q_mtx;
    std::condition_variable q_cv;
    std::deque<Task> queue;
    std::atomic<int> active_tasks{0};

    auto scan_single_dir = [&](DirNode* node, int depth, DirNode* top_bucket, auto& self_ref, bool allow_enqueue) -> void {
        if (cancel_.load()) return;

        dirs_scanned_.fetch_add(1, std::memory_order_relaxed);
        if ((dirs_scanned_.load(std::memory_order_relaxed) & 0x3F) == 0) {
            std::lock_guard<std::mutex> lock(path_mtx_);
            current_path_utf8_ = node->path_utf8;
            elapsed_sec_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }

        std::wstring pattern = JoinPath(node->full_path, L"*");
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            return;
        }

        constexpr uint64_t kLargeFileThreshold = 32ULL * 1024ULL * 1024ULL; // 32 MB files appear as buildings too
        std::vector<std::pair<DirNode*, int>> local_subdirs;

        do {
            if (cancel_.load()) break;
            if (fd.cFileName[0] == L'.' &&
                (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                continue; // skip junctions/symlinks to avoid loops & double counting
            }

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                auto child = std::make_unique<DirNode>();
                child->name = fd.cFileName;
                child->full_path = JoinPath(node->full_path, child->name);
                child->name_utf8 = WideToUtf8(child->name);
                child->path_utf8 = WideToUtf8(child->full_path);
                child->is_dir = true;
                ClassifyNode(*child, depth + 1);

                DirNode* raw_ptr = child.get();
                node->children.push_back(std::move(child));
                local_subdirs.push_back({raw_ptr, depth + 1});
            } else {
                uint64_t fsize = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                node->file_count++;
                files_scanned_.fetch_add(1, std::memory_order_relaxed);
                bytes_scanned_.fetch_add(fsize, std::memory_order_relaxed);
                if (top_bucket) {
                    top_bucket->live_bytes.fetch_add(fsize, std::memory_order_relaxed);
                }

                if (fsize >= kLargeFileThreshold) {
                    auto fnode = std::make_unique<DirNode>();
                    fnode->name = fd.cFileName;
                    fnode->full_path = JoinPath(node->full_path, fnode->name);
                    fnode->name_utf8 = WideToUtf8(fnode->name);
                    fnode->path_utf8 = WideToUtf8(fnode->full_path);
                    fnode->is_dir = false;
                    fnode->total_bytes = fsize;
                    fnode->live_bytes.store(fsize, std::memory_order_relaxed);
                    fnode->file_count = 1;
                    ClassifyNode(*fnode, depth + 1);
                    node->children.push_back(std::move(fnode));
                } else {
                    node->own_file_bytes += fsize;
                }
            }
        } while (FindNextFileW(h, &fd));

        FindClose(h);

        if (cancel_.load()) return;

        if (depth == 0) {
            // Root children are now fixed; expose live_root_ safely before workers start
            for (auto& ch : node->children) {
                if (ch->is_dir) {
                    active_tasks.fetch_add(1, std::memory_order_relaxed);
                    queue.push_back({ch.get(), 1, ch.get()});
                }
            }
            {
                std::lock_guard<std::mutex> lock(tree_mtx_);
                live_root_ = root;
            }
            return;
        }

        if (allow_enqueue && depth < 3 && !local_subdirs.empty()) {
            std::vector<std::pair<DirNode*, int>> to_run_local;
            {
                std::lock_guard<std::mutex> lock(q_mtx);
                for (auto& sub : local_subdirs) {
                    if (queue.size() < 512) {
                        active_tasks.fetch_add(1, std::memory_order_relaxed);
                        queue.push_back({sub.first, sub.second, top_bucket});
                    } else {
                        to_run_local.push_back(sub);
                    }
                }
            }
            q_cv.notify_all();
            for (auto& sub : to_run_local) {
                self_ref(sub.first, sub.second, top_bucket, self_ref, false);
            }
        } else {
            for (auto& sub : local_subdirs) {
                self_ref(sub.first, sub.second, top_bucket, self_ref, false);
            }
        }
    };

    // Scan depth 0 synchronously (takes a few ms) so live_root_->children is immutable for workers & UI
    scan_single_dir(root.get(), 0, nullptr, scan_single_dir, false);

    unsigned int hw = std::thread::hardware_concurrency();
    unsigned int num_workers = std::clamp(hw ? hw : 4u, 4u, 8u);
    std::vector<std::thread> workers;
    workers.reserve(num_workers);

    if (active_tasks.load() > 0 && !cancel_.load()) {
        for (unsigned int i = 0; i < num_workers; ++i) {
            workers.emplace_back([&]() {
                while (true) {
                    Task task{};
                    {
                        std::unique_lock<std::mutex> lock(q_mtx);
                        q_cv.wait(lock, [&]() {
                            return cancel_.load() || !queue.empty() || active_tasks.load() == 0;
                        });
                        if (cancel_.load() || (queue.empty() && active_tasks.load() == 0)) {
                            break;
                        }
                        task = queue.front();
                        queue.pop_front();
                    }

                    scan_single_dir(task.node, task.depth, task.top_bucket, scan_single_dir, true);

                    if (active_tasks.fetch_sub(1) == 1) {
                        q_cv.notify_all();
                    }
                }
            });
        }
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    if (!cancel_.load()) {
        AggregateTree(root.get(), nullptr);
        {
            std::lock_guard<std::mutex> lock(path_mtx_);
            elapsed_sec_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            current_path_utf8_ = root->path_utf8;
        }
        {
            std::lock_guard<std::mutex> lock(tree_mtx_);
            completed_tree_ = std::move(root);
        }
        completed_.store(true);
    } else {
        std::lock_guard<std::mutex> lock(path_mtx_);
        elapsed_sec_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    scanning_.store(false);
}

std::vector<DirNode*> DiskScanner::CollectTopJunk(DirNode* root, size_t max_items) {
    std::vector<DirNode*> junk;
    if (!root) return junk;

    constexpr uint64_t kMinJunkBytes = 10ULL * 1024ULL * 1024ULL; // >= 10 MB
    auto walk = [&](DirNode* n, auto& self) -> void {
        if (!n || n->deleted) return;
        if (n->is_junk && !n->is_protected && n->total_bytes >= kMinJunkBytes) {
            junk.push_back(n);
            return; // don't double-list sub-caches inside an already-junk folder
        }
        for (auto& ch : n->children) {
            self(ch.get(), self);
        }
    };
    walk(root, walk);

    std::sort(junk.begin(), junk.end(), [](const DirNode* a, const DirNode* b) {
        return a->total_bytes > b->total_bytes;
    });
    if (junk.size() > max_items) {
        junk.resize(max_items);
    }
    return junk;
}

bool DiskScanner::MoveToRecycleBin(DirNode* node, uint64_t& out_freed_bytes, std::string& out_error) {
    out_freed_bytes = 0;
    if (!node || node->deleted) {
        out_error = "Объект уже удалён.";
        return false;
    }
    if (node->is_protected) {
        out_error = "Системная папка защищена от удаления!";
        return false;
    }

    // SHFileOperationW requires double-null-terminated path
    std::wstring from = node->full_path;
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;

    int rc = SHFileOperationW(&op);
    if (rc != 0 || op.fAnyOperationsAborted) {
        out_error = "Не удалось переместить в Корзину (код " + std::to_string(rc) + ", возможно файл занят).";
        return false;
    }

    uint64_t freed = node->total_bytes;
    uint64_t files = node->file_count;
    node->deleted = true;
    node->total_bytes = 0;
    node->own_file_bytes = 0;
    node->file_count = 0;

    // Propagate freed size up the tree
    DirNode* p = node->parent;
    while (p) {
        p->total_bytes = (p->total_bytes >= freed) ? (p->total_bytes - freed) : 0;
        p->file_count = (p->file_count >= files) ? (p->file_count - files) : 0;
        p = p->parent;
    }

    out_freed_bytes = freed;
    return true;
}

} // namespace city
