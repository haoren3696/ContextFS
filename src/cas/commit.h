#pragma once
#include "hash.h"
#include <cstdint>
#include <string>
#include <vector>

namespace cas {

struct CommitData {
    Hash tree_hash;
    std::vector<Hash> parents;
    uint64_t session_id;
    uint64_t timestamp_ns;
    std::string label;
    uint32_t policy_version;
};

std::vector<uint8_t> serialize_commit(const CommitData& c);
bool deserialize_commit(const std::vector<uint8_t>& body, CommitData& out);

} // namespace cas
