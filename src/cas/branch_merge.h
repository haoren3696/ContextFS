#pragma once
#include "commit.h"
#include "hash.h"
#include "object_store.h"
#include "working_tree.h"
#include <cstdint>
#include <string>
#include <vector>

namespace cas {

struct MergeResult {
    bool ok = false;
    WorkingTree merged;
    std::vector<std::string> conflicts;
    std::string error;
};

MergeResult merge_trees(
    const WorkingTree& base,
    const WorkingTree& source,
    const WorkingTree& target);

bool load_commit_tree(
    ObjectStore& store,
    const Hash& commit_hash,
    WorkingTree& wt,
    CommitData& commit,
    std::string& error);

bool find_common_ancestor(
    ObjectStore& store,
    const Hash& first,
    const Hash& second,
    Hash& common,
    std::string& error);

Hash write_commit_with_parents(
    ObjectStore& store,
    const WorkingTree& wt,
    const std::vector<Hash>& parents,
    uint64_t session_id,
    uint32_t policy_version,
    const std::string& label,
    std::string& error);

} // namespace cas
