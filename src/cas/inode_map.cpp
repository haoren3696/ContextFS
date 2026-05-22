#include "inode_map.h"

namespace cas {

void InodeMap::set(const InodeKey& k, const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    m_[k] = path;
}

std::string InodeMap::get(const InodeKey& k) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = m_.find(k);
    return (it == m_.end()) ? std::string{} : it->second;
}

void InodeMap::erase_by_path(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = m_.begin(); it != m_.end(); ) {
        if (it->second == path) it = m_.erase(it);
        else ++it;
    }
}

} // namespace cas
