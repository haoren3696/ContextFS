#pragma once
#include "hash.h"
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <optional>
#include <vector>

namespace cas {

enum class EntryKind : uint8_t { Blob = 0, Tree = 1, Symlink = 2, Deleted = 3 };

struct WorkingTreeEntry {
    EntryKind kind;
    Hash hash;
    uint32_t mode;
};

class WorkingTree {
public:
    WorkingTree() = default;
    WorkingTree(WorkingTree&& other) noexcept;
    WorkingTree& operator=(WorkingTree&& other) noexcept;
    WorkingTree(const WorkingTree&) = delete;
    WorkingTree& operator=(const WorkingTree&) = delete;

    std::optional<WorkingTreeEntry> lookup(const std::string& path) const;
    std::optional<WorkingTreeEntry> lookup_raw(const std::string& path) const;
    void insert(const std::string& path, const WorkingTreeEntry& entry);
    void remove(const std::string& path);
    void rename_entry(const std::string& old_path, const std::string& new_path);
    void rename_dir(const std::string& old_prefix, const std::string& new_prefix);

    std::vector<std::pair<std::string, WorkingTreeEntry>> list_dir(const std::string& dir_path) const;

    void clear();
    void for_each(const std::function<void(const std::string&, const WorkingTreeEntry&)>& fn) const;
    void for_each_including_deleted(
        const std::function<void(const std::string&, const WorkingTreeEntry&)>& fn) const;
    size_t size() const;

    WorkingTree clone() const;

    std::mutex& mutex() { return mu_; }

private:
    mutable std::mutex mu_;
    std::map<std::string, WorkingTreeEntry> entries_;
};

} // namespace cas
