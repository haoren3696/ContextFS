#include "daemon.h"
#include "commit.h"
#include "telemetry_registry.h"
#include "tree_serialize.h"
#include "branch_name.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <utility>

namespace cas {

static uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string default_merge_label(
    const std::string& source_name,
    const std::string& target_name,
    const std::string& requested_label) {
    if (!requested_label.empty()) return requested_label;
    return "merge " + source_name + " into " + target_name;
}

Daemon::Daemon(std::string source_root, std::string mount_point, std::string store_root)
    : source_root_(std::move(source_root))
    , mount_point_(std::move(mount_point))
    , store_root_(std::move(store_root))
    , store_(store_root_)
    , refs_(store_root_)
    , cm_(store_, refs_) {
    session_id_ = now_ns();
    main_branch_ = std::make_shared<BranchContext>(0, "main");
    branches_[0] = main_branch_;
}

Daemon::~Daemon() {
    // Defensive: even if main.cpp forgot to call shutdown_telemetry(), this
    // ensures the registry is stopped before its destructor runs (so any
    // FUSE/probe threads have quiesced) and before the rest of Daemon is
    // torn down (working tree, store, etc.) — backends may reference those
    // through callbacks that capture `daemon`.
    shutdown_telemetry();
}

void Daemon::install_registry(std::unique_ptr<TelemetryRegistry> registry) {
    // We expect this to be called once at startup, before FUSE begins
    // serving. If a registry is already installed, stop the old one first
    // to keep teardown sane.
    if (registry_) {
        registry_->stop_all();
    }
    registry_ = std::move(registry);
}

void Daemon::shutdown_telemetry() {
    if (!registry_) return;
    registry_->stop_all();
    registry_.reset();
    // The installer holds a reference into a backend the registry just
    // destroyed; null it so any later accessor sees a defined null state
    // instead of a dangling pointer.
    policy_installer_ = nullptr;
}

bool Daemon::initialize() {
    if (!store_.init_layout()) return false;
    store_.cleanup_tmp();

    Hash current;
    if (!refs_.read_main(current)) {
        std::vector<Hash> written;
        Hash empty_tree = serialize_working_tree(main_branch_->wt, store_, written);
        if (empty_tree == ZERO_HASH) return false;
        CommitData cd;
        cd.tree_hash = empty_tree;
        cd.session_id = session_id_;
        cd.timestamp_ns = now_ns();
        cd.label = "initial";
        cd.policy_version = policy_version_.load();
        Hash commit_hash = store_.write_commit(serialize_commit(cd));
        if (commit_hash == ZERO_HASH) return false;
        std::vector<Hash> to_fsync = store_.drain_pending();
        if (!store_.fsync_objects(to_fsync)) {
            store_.restore_pending(to_fsync);
            return false;
        }
        if (!store_.fsync_shard_dirs(to_fsync)) {
            store_.restore_pending(to_fsync);
            return false;
        }
        if (!refs_.write_main(commit_hash, store_.tmp_dir())) return false;
    }

    std::string error;
    if (!rebuild_main_from_ref(error)) {
        std::fprintf(stderr, "agentvfs: failed to restore main branch: %s\n", error.c_str());
        return false;
    }

    load_persisted_branches();
    return true;
}

bool Daemon::rebuild_branch_from_ref(const std::string& name,
                                     uint32_t branch_id,
                                     std::shared_ptr<BranchContext>& out,
                                     std::string& error) {
    Hash commit_hash;
    if (!refs_.read_ref(name, commit_hash)) {
        error = "failed to read refs/" + name;
        return false;
    }

    std::vector<uint8_t> body;
    if (!store_.read_commit(commit_hash, body)) {
        error = "failed to read commit " + hash_to_hex(commit_hash);
        return false;
    }

    CommitData cd;
    if (!deserialize_commit(body, cd)) {
        error = "failed to parse commit " + hash_to_hex(commit_hash);
        return false;
    }

    WorkingTree wt;
    if (!rebuild_working_tree(cd.tree_hash, store_, wt)) {
        error = "failed to rebuild tree " + hash_to_hex(cd.tree_hash);
        return false;
    }

    out = std::make_shared<BranchContext>(branch_id, name, std::move(wt));
    return true;
}

bool Daemon::rebuild_main_from_ref(std::string& error) {
    std::shared_ptr<BranchContext> rebuilt;
    if (!rebuild_branch_from_ref("main", 0, rebuilt, error)) return false;
    // Move only the WorkingTree into the permanent main_branch_ shared_ptr;
    // the temporary rebuilt shell (and its moved-from wt) is discarded.
    main_branch_->wt = std::move(rebuilt->wt);
    return true;
}

void Daemon::load_persisted_branches() {
    std::vector<std::string> names = refs_.list_refs();

    for (const std::string& name : names) {
        if (name == "main") continue;
        if (!is_valid_branch_name(name)) {
            std::fprintf(stderr,
                         "agentvfs: warning: skipping persisted branch '%s': invalid branch name\n",
                         name.c_str());
            continue;
        }

        std::shared_ptr<BranchContext> branch;
        std::string error;
        uint32_t id = next_branch_id_.fetch_add(1);
        if (!rebuild_branch_from_ref(name, id, branch, error)) {
            std::fprintf(stderr,
                         "agentvfs: warning: skipping persisted branch '%s': %s\n",
                         name.c_str(), error.c_str());
            continue;
        }

        std::lock_guard<std::mutex> lk(branches_mu_);
        bool duplicate = false;
        for (auto& [_, existing] : branches_) {
            if (existing->name == name) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            std::fprintf(stderr,
                         "agentvfs: warning: skipping persisted branch '%s': duplicate branch name\n",
                         name.c_str());
            continue;
        }
        branches_[id] = std::move(branch);
    }
}

uint64_t Daemon::allocate_fh(std::unique_ptr<FhState> state) {
    uint64_t fh = next_fh_id_.fetch_add(1);
    std::lock_guard<std::mutex> lk(fh_table_mu_);
    fh_table_[fh] = std::shared_ptr<FhState>(std::move(state));
    return fh;
}

std::shared_ptr<FhState> Daemon::get_fh(uint64_t fh) {
    std::lock_guard<std::mutex> lk(fh_table_mu_);
    auto it = fh_table_.find(fh);
    return (it == fh_table_.end()) ? nullptr : it->second;
}

void Daemon::release_fh(uint64_t fh) {
    std::lock_guard<std::mutex> lk(fh_table_mu_);
    fh_table_.erase(fh);
}

int64_t Daemon::dirty_fh_size_for_path(const std::string& path, uint32_t branch_id) {
    std::lock_guard<std::mutex> lk(fh_table_mu_);
    for (auto& [_, state] : fh_table_) {
        if (!state || state->stale || !state->write_buf) continue;
        if (state->path != path) continue;
        if (state->branch_id != branch_id) continue;
        if (!state->write_buf->is_dirty()) continue;
        uint64_t base = state->base_cache_loaded ? state->base_cache.size() : state->base_size;
        return (int64_t)state->write_buf->effective_size(base);
    }
    return -1;
}

void Daemon::ensure_base_cache(FhState& s) {
    if (s.base_cache_loaded) return;
    if (s.base_blob != ZERO_HASH) store_.read_blob(s.base_blob, s.base_cache);
    s.base_cache_loaded = true;
}

bool Daemon::flush_fhs_for_branch(uint32_t branch_id) {
    auto br = branch(branch_id);
    if (!br) return false;
    return flush_fhs_for_branch(branch_id, br->wt);
}

bool Daemon::flush_fhs_for_branch(uint32_t branch_id, WorkingTree& wt) {
    std::lock_guard<std::mutex> lk(fh_table_mu_);
    for (auto& [_, state] : fh_table_) {
        if (state->branch_id != branch_id) continue;
        if (state->stale) continue;
        if (!state->write_buf || !state->write_buf->is_dirty()) continue;
        if (!state->base_cache_loaded) {
            if (state->base_blob != ZERO_HASH) store_.read_blob(state->base_blob, state->base_cache);
            state->base_cache_loaded = true;
        }
        auto data = state->write_buf->materialize(state->base_cache);
        Hash new_blob = store_.write_blob(data);
        if (new_blob == ZERO_HASH) return false;
        auto existing = wt.lookup(state->path);
        uint32_t mode = existing ? existing->mode : 0100644;
        wt.insert(state->path, {EntryKind::Blob, new_blob, mode});
        state->write_buf->clear();
        state->base_blob = new_blob;
        state->base_size = data.size();
        state->base_cache = std::move(data);
        state->base_cache_loaded = true;
    }
    return true;
}

void Daemon::invalidate_fhs_for_branch(uint32_t branch_id) {
    std::lock_guard<std::mutex> lk(fh_table_mu_);
    for (auto& [_, state] : fh_table_)
        if (state->branch_id == branch_id) state->stale = true;
}

std::shared_ptr<BranchContext> Daemon::branch(uint32_t id) {
    std::lock_guard<std::mutex> lk(branches_mu_);
    auto it = branches_.find(id);
    return (it != branches_.end()) ? it->second : nullptr;
}

std::shared_ptr<BranchContext> Daemon::branch_by_name(const std::string& name) {
    std::lock_guard<std::mutex> lk(branches_mu_);
    for (auto& [_, br] : branches_)
        if (br->name == name) return br;
    return nullptr;
}

std::shared_ptr<BranchContext> Daemon::branch_by_name_locked(
    const std::string& name,
    std::unique_lock<std::mutex>& checkpoint_lock) {

    checkpoint_lock = std::unique_lock<std::mutex>();
    std::lock_guard<std::mutex> lk(branches_mu_);
    for (auto& [_, br] : branches_) {
        if (br->name != name) continue;
        checkpoint_lock = std::unique_lock<std::mutex>(br->checkpoint_mu);
        return br;
    }
    return nullptr;
}

std::shared_ptr<BranchContext> Daemon::branch_for_pid(Pid pid) {
    uint32_t id = router_.resolve(pid);
    auto br = branch(id);
    return br ? br : main_branch_;
}

uint32_t Daemon::create_branch(const std::string& name, const std::string& from) {
    // Keep branch creation serialized under branches_mu_. This preserves the
    // global lock order used by delete/merge (branches_mu_ -> checkpoint_mu)
    // and prevents duplicate creators from racing ref writes and cleanup.
    std::shared_ptr<BranchContext> src_snap;
    Hash src_commit = ZERO_HASH;
    std::lock_guard<std::mutex> lk(branches_mu_);
    for (auto& [_, br] : branches_) {
        if (br->name == from) { src_snap = br; }
        if (br->name == name) return UINT32_MAX;
    }
    if (!src_snap) return UINT32_MAX;

    uint32_t id = next_branch_id_.fetch_add(1);
    std::shared_ptr<BranchContext> br;
    {
        std::lock_guard<std::mutex> src_lk(src_snap->checkpoint_mu);
        if (!refs_.read_ref(from, src_commit)) return UINT32_MAX;
        br = std::make_shared<BranchContext>(id, name, src_snap->wt.clone());
    }
    if (!refs_.write_ref(name, src_commit, store_.tmp_dir())) return UINT32_MAX;

    branches_[id] = std::move(br);
    return id;
}

BranchMergeResult Daemon::merge_branch(
    const std::string& source_name,
    const std::string& target_name,
    const std::string& label) {

    BranchMergeResult result;
    std::shared_ptr<BranchContext> source;
    std::shared_ptr<BranchContext> target;
    std::unique_lock<std::mutex> first_checkpoint_lk;
    std::unique_lock<std::mutex> second_checkpoint_lk;

    {
        std::unique_lock<std::mutex> branches_lk(branches_mu_);
        for (auto& [_, br] : branches_) {
            if (br->name == source_name) source = br;
            if (br->name == target_name) target = br;
        }
        if (!source || !target) {
            result.error = "unknown branch";
            return result;
        }
        if (source->branch_id == target->branch_id) {
            result.error = "cannot merge branch into itself";
            return result;
        }

        BranchContext* first = source.get();
        BranchContext* second = target.get();
        if (second->branch_id < first->branch_id) std::swap(first, second);

        first_checkpoint_lk = std::unique_lock<std::mutex>(first->checkpoint_mu);
        second_checkpoint_lk = std::unique_lock<std::mutex>(second->checkpoint_mu);
    }

    auto source_cp = cm_.checkpoint_locked(
        "pre-merge " + source_name + " into " + target_name + " source",
        session_id_,
        policy_version_.load(),
        source->wt,
        [this, source] {
            return flush_fhs_for_branch(source->branch_id, source->wt);
        },
        source_name);
    if (!source_cp.ok) {
        result.error = source_cp.error;
        return result;
    }

    auto target_cp = cm_.checkpoint_locked(
        "pre-merge " + source_name + " into " + target_name + " target",
        session_id_,
        policy_version_.load(),
        target->wt,
        [this, target] {
            return flush_fhs_for_branch(target->branch_id, target->wt);
        },
        target_name);
    if (!target_cp.ok) {
        result.error = target_cp.error;
        return result;
    }

    std::string error;
    Hash base_commit = ZERO_HASH;
    if (!find_common_ancestor(store_, source_cp.commit_hash, target_cp.commit_hash,
                              base_commit, error)) {
        result.error = error;
        return result;
    }

    WorkingTree base_wt;
    WorkingTree source_wt;
    WorkingTree target_wt;
    CommitData base_cd;
    CommitData source_cd;
    CommitData target_cd;
    if (!load_commit_tree(store_, base_commit, base_wt, base_cd, error)) {
        result.error = error;
        return result;
    }
    if (!load_commit_tree(store_, source_cp.commit_hash, source_wt, source_cd, error)) {
        result.error = error;
        return result;
    }
    if (!load_commit_tree(store_, target_cp.commit_hash, target_wt, target_cd, error)) {
        result.error = error;
        return result;
    }

    MergeResult merge = merge_trees(base_wt, source_wt, target_wt);
    if (!merge.ok) {
        result.error = merge.error;
        result.conflicts = std::move(merge.conflicts);
        return result;
    }

    Hash merge_commit = write_commit_with_parents(
        store_,
        merge.merged,
        {target_cp.commit_hash, source_cp.commit_hash},
        session_id_,
        policy_version_.load(),
        default_merge_label(source_name, target_name, label),
        error);
    if (merge_commit == ZERO_HASH) {
        result.error = error;
        return result;
    }

    if (!refs_.write_ref(target_name, merge_commit, store_.tmp_dir())) {
        result.error = "failed to advance refs/" + target_name;
        return result;
    }

    target->wt = std::move(merge.merged);
    invalidate_fhs_for_branch(target->branch_id);

    result.ok = true;
    result.commit_hash = merge_commit;
    return result;
}

bool Daemon::delete_branch(const std::string& name) {
    if (name == "main") return false;

    // Extract the branch from the map under branches_mu_. After erase, any
    // other thread holding a shared_ptr keeps the BranchContext alive —
    // the underlying memory stays valid until the last ref drops.
    std::shared_ptr<BranchContext> br;
    {
        std::lock_guard<std::mutex> lk(branches_mu_);
        for (auto& [id, b] : branches_) {
            if (b->name == name) { br = b; break; }
        }
        if (!br) return false;
        if (router_.has_cgroup_for_branch(br->branch_id)) return false;

        // Take the branch's checkpoint_mu *after* branches_mu_: this
        // matches the lock order used by FUSE ops (which never hold
        // branches_mu_) so there is no inversion risk.
        std::lock_guard<std::mutex> br_lk(br->checkpoint_mu);
        invalidate_fhs_for_branch(br->branch_id);
        refs_.remove_ref(name);
        branches_.erase(br->branch_id);
    }
    // br's destructor runs here or later, once every shared_ptr holder is done.
    return true;
}

std::vector<std::shared_ptr<BranchContext>> Daemon::list_branches() {
    std::lock_guard<std::mutex> lk(branches_mu_);
    std::vector<std::shared_ptr<BranchContext>> result;
    for (auto& [_, br] : branches_) result.push_back(br);
    return result;
}

bool Daemon::is_hidden(const std::string& path) {
    return path == "/.agentvfs-store" || path.rfind("/.agentvfs-store/", 0) == 0;
}

} // namespace cas
