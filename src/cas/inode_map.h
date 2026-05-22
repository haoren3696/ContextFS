#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace cas {

struct InodeKey {
    uint64_t dev;
    uint64_t ino;
    uint32_t gen;
    bool operator==(const InodeKey& o) const {
        return dev == o.dev && ino == o.ino && gen == o.gen;
    }
};

struct InodeKeyHash {
    size_t operator()(const InodeKey& k) const noexcept {
        return std::hash<uint64_t>{}(k.dev) ^ (std::hash<uint64_t>{}(k.ino) << 1)
             ^ (std::hash<uint32_t>{}(k.gen) << 2);
    }
};

class InodeMap {
public:
    void set(const InodeKey& k, const std::string& path);
    std::string get(const InodeKey& k) const;
    void erase_by_path(const std::string& path);

private:
    mutable std::mutex mu_;
    std::unordered_map<InodeKey, std::string, InodeKeyHash> m_;
};

} // namespace cas
