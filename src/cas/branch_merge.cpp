#include "branch_merge.h"
#include "tree_serialize.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <unordered_set>
#include <utility>

namespace cas {

namespace {

using EntryState = std::optional<WorkingTreeEntry>;

std::map<std::string, WorkingTreeEntry> to_path_map(const WorkingTree& wt) {
    std::map<std::string, WorkingTreeEntry> out;
    wt.for_each_including_deleted([&](const std::string& path, const WorkingTreeEntry& entry) {
        out[path] = entry;
    });
    return out;
}

EntryState lookup_state(
    const std::map<std::string, WorkingTreeEntry>& entries,
    const std::string& path) {
    auto it = entries.find(path);
    if (it == entries.end()) return std::nullopt;
    return it->second;
}

bool same_entry(const EntryState& a, const EntryState& b) {
    if (!a.has_value() && !b.has_value()) return true;
    if (a.has_value() != b.has_value()) return false;
    if (a->kind != b->kind || a->mode != b->mode) return false;
    if (a->kind == EntryKind::Tree) return true;
    return a->hash == b->hash;
}

bool is_absent(const EntryState& state) {
    return !state.has_value() || state->kind == EntryKind::Deleted;
}

bool same_merge_state(const EntryState& a, const EntryState& b) {
    if (is_absent(a) && is_absent(b)) return true;
    return same_entry(a, b);
}

EntryState equivalent_merge_state(const EntryState& a, const EntryState& b) {
    if (a.has_value() && a->kind == EntryKind::Deleted) return a;
    if (b.has_value() && b->kind == EntryKind::Deleted) return b;
    return a.has_value() ? a : b;
}

void apply_state(WorkingTree& wt, const std::string& path, const EntryState& state) {
    if (state.has_value()) wt.insert(path, *state);
}

uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool read_commit_data(
    ObjectStore& store,
    const Hash& hash,
    CommitData& out,
    std::string& error) {
    std::vector<uint8_t> body;
    if (!store.read_commit(hash, body)) {
        error = "failed to read commit " + hash_to_hex(hash);
        return false;
    }
    if (!deserialize_commit(body, out)) {
        error = "failed to parse commit " + hash_to_hex(hash);
        return false;
    }
    return true;
}

using AncestorMap = std::map<std::string, Hash>;

bool collect_ancestors(
    ObjectStore& store,
    const Hash& start,
    AncestorMap& ancestors,
    std::string& error) {
    std::deque<Hash> q;
    q.push_back(start);

    while (!q.empty()) {
        Hash h = q.front();
        q.pop_front();
        std::string hex = hash_to_hex(h);
        if (!ancestors.emplace(hex, h).second) continue;

        CommitData cd;
        if (!read_commit_data(store, h, cd, error)) return false;
        for (const Hash& parent : cd.parents) q.push_back(parent);
    }
    return true;
}

bool is_ancestor_of(
    ObjectStore& store,
    const Hash& candidate,
    const Hash& descendant,
    std::string& error) {
    std::string candidate_hex = hash_to_hex(candidate);
    std::unordered_set<std::string> seen;
    std::deque<Hash> q;
    q.push_back(descendant);

    while (!q.empty()) {
        Hash h = q.front();
        q.pop_front();
        std::string hex = hash_to_hex(h);
        if (!seen.insert(hex).second) continue;
        if (hex == candidate_hex) return true;

        CommitData cd;
        if (!read_commit_data(store, h, cd, error)) return false;
        for (const Hash& parent : cd.parents) q.push_back(parent);
    }
    return false;
}

std::string parent_path(const std::string& path) {
    auto slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return {};
    return path.substr(0, slash);
}

bool is_tree(const EntryState& state) {
    return state.has_value() && state->kind == EntryKind::Tree;
}

void detect_structural_conflicts(
    const std::map<std::string, EntryState>& merged_states,
    std::vector<std::string>& conflicts) {

    for (const auto& [path, state] : merged_states) {
        if (!state.has_value() || state->kind == EntryKind::Deleted) continue;

        for (std::string ancestor = parent_path(path);
             !ancestor.empty();
             ancestor = parent_path(ancestor)) {
            auto it = merged_states.find(ancestor);
            if (it != merged_states.end() && !is_tree(it->second)) {
                conflicts.push_back(ancestor);
                break;
            }
        }
    }
}

} // namespace

MergeResult merge_trees(
    const WorkingTree& base,
    const WorkingTree& source,
    const WorkingTree& target) {

    auto base_entries = to_path_map(base);
    auto source_entries = to_path_map(source);
    auto target_entries = to_path_map(target);

    std::vector<std::string> paths;
    for (const auto& [path, _] : base_entries) paths.push_back(path);
    for (const auto& [path, _] : source_entries) paths.push_back(path);
    for (const auto& [path, _] : target_entries) paths.push_back(path);
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

    std::map<std::string, EntryState> merged_states;
    std::vector<std::string> conflicts;

    for (const std::string& path : paths) {
        EntryState base_state = lookup_state(base_entries, path);
        EntryState source_state = lookup_state(source_entries, path);
        EntryState target_state = lookup_state(target_entries, path);

        bool source_changed = !same_entry(base_state, source_state);
        bool target_changed = !same_entry(base_state, target_state);

        if (!source_changed && !target_changed) {
            merged_states[path] = base_state;
        } else if (!source_changed) {
            merged_states[path] = target_state;
        } else if (!target_changed) {
            merged_states[path] = source_state;
        } else if (same_merge_state(source_state, target_state)) {
            merged_states[path] = equivalent_merge_state(source_state, target_state);
        } else {
            conflicts.push_back(path);
        }
    }

    detect_structural_conflicts(merged_states, conflicts);
    std::sort(conflicts.begin(), conflicts.end());
    conflicts.erase(std::unique(conflicts.begin(), conflicts.end()), conflicts.end());

    MergeResult result;
    result.ok = conflicts.empty();
    if (!result.ok) {
        result.error = "merge conflicts";
        result.conflicts = std::move(conflicts);
        result.merged.clear();
        return result;
    }

    for (const auto& [path, state] : merged_states) {
        apply_state(result.merged, path, state);
    }
    return result;
}

bool load_commit_tree(
    ObjectStore& store,
    const Hash& commit_hash,
    WorkingTree& wt,
    CommitData& commit,
    std::string& error) {
    CommitData loaded_commit;
    if (!read_commit_data(store, commit_hash, loaded_commit, error)) return false;

    WorkingTree loaded_wt;
    if (!rebuild_working_tree(loaded_commit.tree_hash, store, loaded_wt)) {
        error = "failed to rebuild tree " + hash_to_hex(loaded_commit.tree_hash);
        return false;
    }

    wt = std::move(loaded_wt);
    commit = std::move(loaded_commit);
    error.clear();
    return true;
}

bool find_common_ancestor(
    ObjectStore& store,
    const Hash& first,
    const Hash& second,
    Hash& common,
    std::string& error) {

    common = ZERO_HASH;
    if (first == ZERO_HASH || second == ZERO_HASH) {
        error = "no common ancestor";
        return false;
    }

    AncestorMap first_ancestors;
    if (!collect_ancestors(store, first, first_ancestors, error)) return false;

    AncestorMap second_ancestors;
    if (!collect_ancestors(store, second, second_ancestors, error)) return false;

    std::vector<Hash> candidates;
    for (const auto& [hex, hash] : first_ancestors) {
        if (second_ancestors.find(hex) != second_ancestors.end()) {
            candidates.push_back(hash);
        }
    }

    if (candidates.empty()) {
        error = "no common ancestor";
        return false;
    }

    std::vector<std::pair<std::string, Hash>> merge_bases;
    for (const Hash& candidate : candidates) {
        bool superseded = false;
        for (const Hash& other : candidates) {
            if (candidate == other) continue;
            if (is_ancestor_of(store, candidate, other, error)) {
                superseded = true;
                break;
            }
        }
        if (!superseded) merge_bases.push_back({hash_to_hex(candidate), candidate});
    }

    // Multiple best merge bases can exist in criss-cross histories. Pick the
    // lexicographically lowest hash so callers get a stable result.
    std::sort(merge_bases.begin(), merge_bases.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    common = merge_bases.front().second;
    error.clear();
    return true;
}

Hash write_commit_with_parents(
    ObjectStore& store,
    const WorkingTree& wt,
    const std::vector<Hash>& parents,
    uint64_t session_id,
    uint32_t policy_version,
    const std::string& label,
    std::string& error) {

    std::vector<Hash> written_hashes;
    Hash tree_hash = serialize_working_tree(wt, store, written_hashes);
    if (tree_hash == ZERO_HASH) {
        error = "failed to serialize merged tree";
        return ZERO_HASH;
    }

    CommitData cd;
    cd.tree_hash = tree_hash;
    cd.parents = parents;
    cd.session_id = session_id;
    cd.timestamp_ns = now_ns();
    cd.label = label;
    cd.policy_version = policy_version;

    Hash commit_hash = store.write_commit(serialize_commit(cd));
    if (commit_hash == ZERO_HASH) {
        error = "failed to write commit object";
        return ZERO_HASH;
    }

    // Same approach as CheckpointManager::checkpoint_locked: drain pending
    // writes, then narrow to objects reachable from this merge commit, so we
    // skip both already-durable leaves and intra-window orphans.
    std::vector<Hash> pending = store.drain_pending();
    struct HashHasher {
        size_t operator()(const Hash& h) const noexcept {
            size_t v;
            std::memcpy(&v, h.data(), sizeof(v));
            return v;
        }
    };
    std::unordered_set<Hash, HashHasher> referenced;
    referenced.reserve(pending.size() * 2);
    wt.for_each_including_deleted([&](const std::string&,
                                       const WorkingTreeEntry& e) {
        if ((e.kind == EntryKind::Blob || e.kind == EntryKind::Symlink) &&
            e.hash != ZERO_HASH) {
            referenced.insert(e.hash);
        }
    });
    for (const auto& h : written_hashes) referenced.insert(h);
    referenced.insert(commit_hash);

    std::vector<Hash> to_fsync;
    to_fsync.reserve(pending.size());
    for (const auto& h : pending) {
        if (referenced.count(h)) to_fsync.push_back(h);
    }

    if (!store.fsync_objects(to_fsync)) {
        store.restore_pending(pending);
        error = "failed to fsync merge objects";
        return ZERO_HASH;
    }
    if (!store.fsync_shard_dirs(to_fsync)) {
        store.restore_pending(pending);
        error = "failed to fsync merge shard dirs";
        return ZERO_HASH;
    }

    error.clear();
    return commit_hash;
}

} // namespace cas
