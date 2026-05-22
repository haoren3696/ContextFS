#include "working_tree.h"
#include <algorithm>

namespace cas {

std::optional<WorkingTreeEntry> WorkingTree::lookup(const std::string& path) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(path);
    if (it == entries_.end()) return std::nullopt;
    if (it->second.kind == EntryKind::Deleted) return std::nullopt;
    return it->second;
}

std::optional<WorkingTreeEntry> WorkingTree::lookup_raw(const std::string& path) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(path);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

void WorkingTree::insert(const std::string& path, const WorkingTreeEntry& entry) {
    std::lock_guard<std::mutex> lk(mu_);
    entries_[path] = entry;
}

void WorkingTree::remove(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    entries_[path] = {EntryKind::Deleted, ZERO_HASH, 0};
}

void WorkingTree::rename_entry(const std::string& old_path, const std::string& new_path) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(old_path);
    if (it == entries_.end()) return;
    WorkingTreeEntry e = it->second;
    entries_.erase(it);
    entries_[old_path] = {EntryKind::Deleted, ZERO_HASH, 0};
    entries_[new_path] = e;
}

void WorkingTree::rename_dir(const std::string& old_prefix, const std::string& new_prefix) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::pair<std::string, WorkingTreeEntry>> to_move;
    std::string scan = old_prefix;
    if (!scan.empty() && scan.back() != '/') scan += '/';

    for (auto it = entries_.lower_bound(scan); it != entries_.end(); ++it) {
        if (it->first.compare(0, scan.size(), scan) != 0) break;
        to_move.push_back(*it);
    }
    // Also move the directory entry itself
    auto dir_it = entries_.find(old_prefix);
    if (dir_it != entries_.end()) to_move.push_back(*dir_it);

    for (auto& [path, entry] : to_move) {
        entries_[path] = {EntryKind::Deleted, ZERO_HASH, 0};
        std::string new_path;
        if (path == old_prefix) {
            new_path = new_prefix;
        } else {
            new_path = new_prefix + path.substr(old_prefix.size());
        }
        entries_[new_path] = entry;
    }
}

std::vector<std::pair<std::string, WorkingTreeEntry>> WorkingTree::list_dir(const std::string& dir_path) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::pair<std::string, WorkingTreeEntry>> result;
    std::string prefix = dir_path;
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';

    for (auto it = entries_.lower_bound(prefix); it != entries_.end(); ++it) {
        if (it->first.compare(0, prefix.size(), prefix) != 0) break;
        if (it->second.kind == EntryKind::Deleted) continue;
        // Only direct children: no additional '/' after prefix
        auto rest = it->first.substr(prefix.size());
        if (rest.find('/') != std::string::npos) continue;
        result.push_back(*it);
    }
    return result;
}

void WorkingTree::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.clear();
}

void WorkingTree::for_each(const std::function<void(const std::string&, const WorkingTreeEntry&)>& fn) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [path, entry] : entries_) {
        if (entry.kind != EntryKind::Deleted) fn(path, entry);
    }
}

void WorkingTree::for_each_including_deleted(
    const std::function<void(const std::string&, const WorkingTreeEntry&)>& fn) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [path, entry] : entries_) fn(path, entry);
}

size_t WorkingTree::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t count = 0;
    for (auto& [_, e] : entries_)
        if (e.kind != EntryKind::Deleted) count++;
    return count;
}

WorkingTree::WorkingTree(WorkingTree&& other) noexcept {
    std::lock_guard<std::mutex> lk(other.mu_);
    entries_ = std::move(other.entries_);
}

WorkingTree& WorkingTree::operator=(WorkingTree&& other) noexcept {
    if (this == &other) return *this;
    std::lock(mu_, other.mu_);
    std::lock_guard<std::mutex> lk1(mu_, std::adopt_lock);
    std::lock_guard<std::mutex> lk2(other.mu_, std::adopt_lock);
    entries_ = std::move(other.entries_);
    return *this;
}

WorkingTree WorkingTree::clone() const {
    std::lock_guard<std::mutex> lk(mu_);
    WorkingTree copy;
    copy.entries_ = entries_;
    return copy;
}

} // namespace cas
