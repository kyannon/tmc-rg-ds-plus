#pragma once
/*
 * Address -> loaded-asset-blob lookup.
 *
 * The port asks "does this pointer lie inside an asset blob we loaded?" on
 * every DMA / MemCopy source, every animation read and every LZ77 source
 * bound (Port_IsLoadedAssetBytes / Port_LoadedAssetBytesEnd). That used to
 * walk every entry of the asset cache's unordered_map — thousands of
 * node + vector indirections per call, growing as the game loads assets.
 * On the RG DS Plus (in-order Cortex-A55) it was >90% of the game thread's
 * time in the field.
 *
 * The cache is insert-only and blobs never change after insertion, so this
 * keeps their [begin, end) ranges sorted and rebuilds only when the number
 * of cached blobs changes; a lookup is a binary search.
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace port_blob_index {

struct Range {
    const uint8_t* begin;
    const uint8_t* end;
};

class Index {
  public:
    /* Rebuild from any container of (key, owning pointer to vector<u8>)
     * pairs when its size changed since the last build. */
    template <typename Map> void Sync(const Map& blobs) {
        if (built_ && blobs.size() == count_) {
            return;
        }
        ranges_.clear();
        ranges_.reserve(blobs.size());
        for (const auto& kv : blobs) {
            const auto& v = kv.second;
            if (v == nullptr || v->empty()) {
                continue;
            }
            ranges_.push_back({ v->data(), v->data() + v->size() });
        }
        std::sort(ranges_.begin(), ranges_.end(),
                  [](const Range& a, const Range& b) { return a.begin < b.begin; });
        count_ = blobs.size();
        built_ = true;
    }

    /* The blob with the greatest begin <= at, or nullptr. */
    const Range* Floor(const uint8_t* at) const {
        auto it = std::upper_bound(ranges_.begin(), ranges_.end(), at,
                                   [](const uint8_t* p, const Range& r) { return p < r.begin; });
        if (it == ranges_.begin()) {
            return nullptr;
        }
        return &*(it - 1);
    }

    /* Port_IsLoadedAssetBytes semantics: at in [begin, end] and size bytes fit. */
    bool Contains(const void* ptr, uint32_t size) const {
        const uint8_t* at = static_cast<const uint8_t*>(ptr);
        const Range* r = Floor(at);
        return r != nullptr && at <= r->end && size <= static_cast<uint32_t>(r->end - at);
    }

    /* Port_LoadedAssetBytesEnd semantics: one-past-the-end of the blob
     * holding `ptr` (at in [begin, end)), or nullptr. */
    const uint8_t* EndOf(const void* ptr) const {
        const uint8_t* at = static_cast<const uint8_t*>(ptr);
        const Range* r = Floor(at);
        return (r != nullptr && at < r->end) ? r->end : nullptr;
    }

    size_t size() const { return ranges_.size(); }

  private:
    std::vector<Range> ranges_;
    size_t count_ = 0;
    bool built_ = false;
};

} // namespace port_blob_index
