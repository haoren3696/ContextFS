#include "write_buffer.h"
#include <algorithm>
#include <cstring>

namespace cas {

WriteBuffer::WriteBuffer(const Hash& base_blob, uint64_t base_size)
    : base_blob_(base_blob), base_size_(base_size) {}

void WriteBuffer::write(uint64_t offset, const uint8_t* data, size_t len) {
    if (len == 0) return;

    uint64_t new_end = offset + len;

    // Find overlapping/adjacent entries to coalesce
    auto it = overlay_.upper_bound(offset);
    if (it != overlay_.begin()) {
        --it;
        uint64_t prev_end = it->first + it->second.size();
        if (prev_end < offset) {
            ++it; // no overlap with previous
        }
    }

    // Determine merged range
    uint64_t merge_start = offset;
    uint64_t merge_end = new_end;
    if (it != overlay_.end() && it->first < merge_start)
        merge_start = it->first;

    // Collect all overlapping/adjacent entries
    auto erase_begin = it;
    auto erase_end = it;
    uint64_t old_dirty = 0;
    while (erase_end != overlay_.end()) {
        uint64_t e_start = erase_end->first;
        uint64_t e_end = e_start + erase_end->second.size();
        if (e_start > merge_end) break;
        if (e_end > merge_end) merge_end = e_end;
        old_dirty += erase_end->second.size();
        ++erase_end;
    }

    // Build merged buffer
    std::vector<uint8_t> merged(merge_end - merge_start);

    // Copy existing overlay data
    for (auto jt = erase_begin; jt != erase_end; ++jt) {
        std::memcpy(merged.data() + (jt->first - merge_start),
                     jt->second.data(), jt->second.size());
    }

    // Overwrite with new data
    std::memcpy(merged.data() + (offset - merge_start), data, len);

    // Erase old entries, insert merged
    overlay_.erase(erase_begin, erase_end);
    overlay_[merge_start] = std::move(merged);

    dirty_bytes_ = dirty_bytes_ - old_dirty + overlay_[merge_start].size();

    // Extend effective size if write goes past end
    uint64_t eff = effective_size(base_size_);
    if (new_end > eff) {
        size_override_ = new_end;
    }
}

size_t WriteBuffer::read(uint64_t offset, uint8_t* buf, size_t len,
                          const std::vector<uint8_t>& base_data) const {
    uint64_t eff = effective_size(base_data.size());
    if (offset >= eff) return 0;
    size_t avail = std::min(len, (size_t)(eff - offset));

    // Start with base data or zeros
    for (size_t i = 0; i < avail; i++) {
        uint64_t pos = offset + i;
        if (pos < base_data.size())
            buf[i] = base_data[pos];
        else
            buf[i] = 0;
    }

    // Apply overlay
    for (auto& [ov_off, ov_data] : overlay_) {
        uint64_t ov_end = ov_off + ov_data.size();
        uint64_t req_end = offset + avail;
        if (ov_end <= offset || ov_off >= req_end) continue;

        uint64_t copy_start = std::max(ov_off, offset);
        uint64_t copy_end = std::min(ov_end, req_end);
        std::memcpy(buf + (copy_start - offset),
                     ov_data.data() + (copy_start - ov_off),
                     copy_end - copy_start);
    }

    return avail;
}

void WriteBuffer::truncate(uint64_t new_size) {
    size_override_ = new_size;
    // Trim overlay entries past new_size
    auto it = overlay_.lower_bound(new_size);
    while (it != overlay_.end()) {
        dirty_bytes_ -= it->second.size();
        it = overlay_.erase(it);
    }
    // Trim partial overlap
    if (!overlay_.empty()) {
        auto last = std::prev(overlay_.end());
        uint64_t end = last->first + last->second.size();
        if (end > new_size) {
            dirty_bytes_ -= last->second.size();
            last->second.resize(new_size - last->first);
            dirty_bytes_ += last->second.size();
        }
    }
}

uint64_t WriteBuffer::effective_size(uint64_t base_size) const {
    if (size_override_.has_value()) return *size_override_;
    uint64_t max_from_overlay = base_size;
    if (!overlay_.empty()) {
        auto last = std::prev(overlay_.end());
        uint64_t end = last->first + last->second.size();
        if (end > max_from_overlay) max_from_overlay = end;
    }
    return max_from_overlay;
}

std::vector<uint8_t> WriteBuffer::materialize(const std::vector<uint8_t>& base_data) const {
    uint64_t eff = effective_size(base_data.size());
    std::vector<uint8_t> result(eff);
    if (eff > 0) {
        size_t copy_len = std::min((size_t)eff, base_data.size());
        std::memcpy(result.data(), base_data.data(), copy_len);
    }
    for (auto& [ov_off, ov_data] : overlay_) {
        size_t copy_len = std::min(ov_data.size(), (size_t)(eff - ov_off));
        std::memcpy(result.data() + ov_off, ov_data.data(), copy_len);
    }
    return result;
}

void WriteBuffer::clear() {
    overlay_.clear();
    size_override_.reset();
    dirty_bytes_ = 0;
}

} // namespace cas
