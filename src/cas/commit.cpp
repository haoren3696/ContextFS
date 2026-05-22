#include "commit.h"
#include <cstring>

namespace cas {

std::vector<uint8_t> serialize_commit(const CommitData& c) {
    size_t sz = 32 + 4 + c.parents.size() * 32 + 8 + 8 + 2 + c.label.size() + 4;
    std::vector<uint8_t> buf(sz);
    size_t pos = 0;

    std::memcpy(buf.data() + pos, c.tree_hash.data(), 32); pos += 32;
    uint32_t pc = c.parents.size();
    std::memcpy(buf.data() + pos, &pc, 4); pos += 4;
    for (auto& p : c.parents) {
        std::memcpy(buf.data() + pos, p.data(), 32); pos += 32;
    }
    std::memcpy(buf.data() + pos, &c.session_id, 8); pos += 8;
    std::memcpy(buf.data() + pos, &c.timestamp_ns, 8); pos += 8;
    uint16_t ll = c.label.size();
    std::memcpy(buf.data() + pos, &ll, 2); pos += 2;
    if (ll > 0) { std::memcpy(buf.data() + pos, c.label.data(), ll); pos += ll; }
    std::memcpy(buf.data() + pos, &c.policy_version, 4);

    return buf;
}

bool deserialize_commit(const std::vector<uint8_t>& body, CommitData& out) {
    if (body.size() < 32 + 4) return false;
    size_t pos = 0;

    std::memcpy(out.tree_hash.data(), body.data() + pos, 32); pos += 32;
    uint32_t pc;
    std::memcpy(&pc, body.data() + pos, 4); pos += 4;
    if (body.size() < pos + pc * 32 + 8 + 8 + 2 + 4) return false;
    out.parents.resize(pc);
    for (uint32_t i = 0; i < pc; i++) {
        std::memcpy(out.parents[i].data(), body.data() + pos, 32); pos += 32;
    }
    std::memcpy(&out.session_id, body.data() + pos, 8); pos += 8;
    std::memcpy(&out.timestamp_ns, body.data() + pos, 8); pos += 8;
    uint16_t ll;
    std::memcpy(&ll, body.data() + pos, 2); pos += 2;
    if (body.size() < pos + ll + 4) return false;
    out.label.assign(reinterpret_cast<const char*>(body.data() + pos), ll); pos += ll;
    std::memcpy(&out.policy_version, body.data() + pos, 4);

    return true;
}

} // namespace cas
