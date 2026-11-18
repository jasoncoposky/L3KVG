#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
#include <deque>
#include <shared_mutex>
#include <unordered_map>
#include <array>
#include <type_traits>
#include <functional>
#include <memory>
#include <limits>
#include <sys/mman.h>
#include "L3KVG/PackedEdge.hpp"
#include "L3KVG/LabelRegistry.hpp"
#include "L3KVG/SharedSpinLock.hpp"

namespace l3kvg {

struct alignas(8) VertexRecord {
    static constexpr uint32_t FLAG_PROMOTED = 1u << 31;
    static constexpr uint32_t DEGREE_MASK = ~FLAG_PROMOTED;

    uint32_t out_degree{0};
    uint32_t in_degree{0};
    union {
        PackedEdge inline_edge; // Inlined edge when out_degree > 0 && !is_promoted()
        struct {
            uint32_t out_block_id; // 1-indexed into page_pool (0 = none)
            uint32_t in_block_id;  // 1-indexed into page_pool (0 = none)
            uint64_t page_offset;
        } ext;
    };

    VertexRecord() noexcept : out_degree(0), in_degree(0) {
        ext.out_block_id = 0;
        ext.in_block_id = 0;
        ext.page_offset = 0;
    }

    [[nodiscard]] bool is_promoted() const noexcept {
        return (out_degree & FLAG_PROMOTED) != 0;
    }

    [[nodiscard]] bool is_inlined() const noexcept {
        return !is_promoted() && (out_degree > 0);
    }

    [[nodiscard]] uint32_t get_out_degree() const noexcept {
        return out_degree & DEGREE_MASK;
    }
};

static_assert(sizeof(VertexRecord) == 32, "VertexRecord must be exactly 32 bytes");
static_assert(alignof(VertexRecord) == 8, "VertexRecord must be 8-byte aligned");
static_assert(std::is_trivially_copyable_v<VertexRecord>, "VertexRecord must be trivially copyable");

struct alignas(64) AdjacencyPage {
    static constexpr uint32_t CAPACITY = 170; // 170 * 24 = 4080 bytes. 4080 + 16 header = 4096 bytes (4KB)
    uint32_t count{0};
    uint32_t next_page_id{0}; // 0 = none, or 1-indexed into page_pool
    uint32_t version{0};
    uint32_t reserved{0};
    PackedEdge edges[CAPACITY];

    AdjacencyPage() noexcept : count(0), next_page_id(0), version(0), reserved(0) {}
};
static_assert(sizeof(AdjacencyPage) == 4096, "AdjacencyPage must be exactly 4096 bytes");
static_assert(alignof(AdjacencyPage) == 64, "AdjacencyPage must be 64-byte aligned");

class AdjacencyStore {
public:
    static constexpr size_t NUM_SHARDS = 64;

    [[nodiscard]] static constexpr uint32_t decode_shard_index(uint32_t page_id) noexcept {
        return (page_id >> 24) & 0xFF;
    }

    [[nodiscard]] static constexpr uint32_t decode_local_page_id(uint32_t page_id) noexcept {
        return page_id & 0x00FFFFFF;
    }

    [[nodiscard]] static constexpr uint32_t encode_page_id(size_t shard_idx, uint32_t local_idx) noexcept {
        return (static_cast<uint32_t>(shard_idx) << 24) | (local_idx & 0x00FFFFFF);
    }

    [[nodiscard]] static size_t shard_index(uint64_t id) noexcept {
        uint64_t x = id;
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x % NUM_SHARDS;
    }

    AdjacencyStore();
    ~AdjacencyStore() = default;

    AdjacencyStore(const AdjacencyStore&) = delete;
    AdjacencyStore& operator=(const AdjacencyStore&) = delete;
    AdjacencyStore(AdjacencyStore&&) = delete;
    AdjacencyStore& operator=(AdjacencyStore&&) = delete;

    bool add_edge(uint64_t src_id, uint64_t dst_id, uint16_t label_id,
                  float weight = 1.0f, uint16_t flags = PackedEdge::FLAG_ACTIVE,
                  uint32_t prop_index = 0, uint64_t hlc = 0);

    bool del_edge(uint64_t src_id, uint64_t dst_id, uint16_t label_id,
                  float weight = -1.0f,
                  uint16_t* out_removed_flags = nullptr,
                  float* out_removed_weight = nullptr);

    struct IncidentEdgeRecord {
        uint64_t src_id{0};
        uint64_t dst_id{0};
        uint16_t label_id{0};
        uint16_t flags{0};
        float weight{1.0f};
    };

    [[nodiscard]] std::vector<IncidentEdgeRecord> remove_vertex(uint64_t id);

    template <typename ReserveFn, typename VisitFn>
    void snapshot_active_edges(ReserveFn&& reserve_fn, VisitFn&& visitor) const {
        std::array<std::shared_lock<SharedSpinLock>, NUM_SHARDS> locks;
        for (size_t s_idx = 0; s_idx < NUM_SHARDS; ++s_idx) {
            locks[s_idx] = std::shared_lock<SharedSpinLock>(shards_[s_idx].mutex);
        }

        size_t total = 0;
        for (size_t s_idx = 0; s_idx < NUM_SHARDS; ++s_idx) {
            for (const auto& [_, v] : shards_[s_idx].directory) {
                total += v.get_out_degree();
            }
        }
        reserve_fn(total);

        for (size_t s_idx = 0; s_idx < NUM_SHARDS; ++s_idx) {
            const auto& shard = shards_[s_idx];
            for (const auto& [src_id, v] : shard.directory) {
                if (v.is_inlined()) {
                    if (v.inline_edge.is_active()) {
                        visitor(src_id, v.inline_edge);
                    }
                    continue;
                }
                if (!v.is_promoted()) continue;
                uint32_t curr_id = v.ext.out_block_id;
                while (curr_id != 0) {
                    uint32_t p_shard = decode_shard_index(curr_id);
                    uint32_t p_local = decode_local_page_id(curr_id);
                    if (p_shard != s_idx || p_local >= shard.page_pool.size()) break;
                    const auto& page = shard.page_pool[p_local];
                    if (page.next_page_id == curr_id) break;
                    for (uint32_t i = 0; i < page.count; ++i) {
                        if (page.edges[i].is_active()) {
                            visitor(src_id, page.edges[i]);
                        }
                    }
                    curr_id = page.next_page_id;
                }
            }
        }
    }

    template <typename F>
    void for_each_active_edge(F&& visitor) const {
        for (size_t s_idx = 0; s_idx < NUM_SHARDS; ++s_idx) {
            const auto& shard = shards_[s_idx];
            std::shared_lock lock(shard.mutex);
            for (const auto& [src_id, v] : shard.directory) {
                if (v.is_inlined()) {
                    if (v.inline_edge.is_active()) {
                        visitor(src_id, v.inline_edge);
                    }
                    continue;
                }
                if (!v.is_promoted()) continue;
                uint32_t curr_id = v.ext.out_block_id;
                while (curr_id != 0) {
                    uint32_t p_shard = decode_shard_index(curr_id);
                    uint32_t p_local = decode_local_page_id(curr_id);
                    if (p_shard != s_idx || p_local >= shard.page_pool.size()) break;
                    const auto& page = shard.page_pool[p_local];
                    if (page.next_page_id == curr_id) break;
                    for (uint32_t i = 0; i < page.count; ++i) {
                        if (page.edges[i].is_active()) {
                            visitor(src_id, page.edges[i]);
                        }
                    }
                    curr_id = page.next_page_id;
                }
            }
        }
    }

    [[nodiscard]] size_t total_active_edges() const noexcept {
        size_t total = 0;
        for (size_t s_idx = 0; s_idx < NUM_SHARDS; ++s_idx) {
            const auto& shard = shards_[s_idx];
            std::shared_lock lock(shard.mutex);
            for (const auto& [_, v] : shard.directory) {
                total += v.get_out_degree();
            }
        }
        return total;
    }

    template <typename F>
    void for_each_outgoing_edge(uint64_t src_id, F&& visitor) const {
        size_t s_idx = shard_index(src_id);
        const auto& shard = shards_[s_idx];
        std::shared_lock lock(shard.mutex);
        auto it = shard.directory.find(src_id);
        if (it == shard.directory.end()) return;
        const auto& v = it->second;
        if (v.is_inlined()) {
            visitor(v.inline_edge);
            return;
        }
        if (!v.is_promoted()) return;
        uint32_t curr_id = v.ext.out_block_id;
        while (curr_id != 0) {
            uint32_t p_shard = decode_shard_index(curr_id);
            uint32_t p_local = decode_local_page_id(curr_id);
            if (p_shard != s_idx || p_local >= shard.page_pool.size()) break;
            const auto& page = shard.page_pool[p_local];
            if (page.next_page_id == curr_id) break; // cycle guard
            for (uint32_t i = 0; i < page.count; ++i) {
                visitor(page.edges[i]);
            }
            curr_id = page.next_page_id;
        }
    }

    template <typename F>
    void for_each_incoming_edge(uint64_t dst_id, F&& visitor) const {
        size_t s_idx = shard_index(dst_id);
        const auto& shard = shards_[s_idx];
        std::shared_lock lock(shard.mutex);
        auto it = shard.directory.find(dst_id);
        if (it == shard.directory.end()) return;
        const auto& v = it->second;
        if (!v.is_promoted()) return;
        uint32_t curr_id = v.ext.in_block_id;
        while (curr_id != 0) {
            uint32_t p_shard = decode_shard_index(curr_id);
            uint32_t p_local = decode_local_page_id(curr_id);
            if (p_shard != s_idx || p_local >= shard.page_pool.size()) break;
            const auto& page = shard.page_pool[p_local];
            if (page.next_page_id == curr_id) break; // cycle guard
            for (uint32_t i = 0; i < page.count; ++i) {
                visitor(page.edges[i]);
            }
            curr_id = page.next_page_id;
        }
    }

    [[nodiscard]] std::vector<PackedEdge> get_outgoing_edges_snapshot(uint64_t src_id) const;
    [[nodiscard]] std::vector<PackedEdge> get_incoming_edges_snapshot(uint64_t dst_id) const;
    void filter_outgoing_edges(uint64_t src_id, uint16_t target_label, float min_weight, std::vector<uint64_t>& out_neighbors) const;

    /**
     * @brief Note: get_outgoing_edges returns a span of the first block only.
     * For multi-page edge chains and thread-safe traversal, for_each_outgoing_edge
     * or get_outgoing_edges_snapshot is the canonical API.
     */
    [[nodiscard]] std::span<const PackedEdge> get_outgoing_edges(uint64_t src_id) const noexcept;

    /**
     * @brief Note: get_incoming_edges returns a span of the first block only.
     * For multi-page edge chains and thread-safe traversal, for_each_incoming_edge
     * or get_incoming_edges_snapshot is the canonical API.
     */
    [[nodiscard]] std::span<const PackedEdge> get_incoming_edges(uint64_t dst_id) const noexcept;

    [[nodiscard]] size_t get_out_degree(uint64_t src_id) const noexcept;
    [[nodiscard]] size_t get_in_degree(uint64_t dst_id) const noexcept;

    // Helper for testing page internals
    [[nodiscard]] uint32_t get_out_page_id(uint64_t src_id) const noexcept;
    [[nodiscard]] const AdjacencyPage* get_page(uint32_t page_id) const noexcept;

    class PagePool {
    public:
        static constexpr size_t CHUNK_SIZE = 512; // 512 pages * 4KB = 2MB THP-aligned chunk

        PagePool() {
            allocate_page(); // Reserve index 0 as null page
        }

        ~PagePool() {
            for (auto* chunk : chunks_) {
                ::munmap(chunk, sizeof(AdjacencyPage) * CHUNK_SIZE);
            }
        }

        PagePool(const PagePool&) = delete;
        PagePool& operator=(const PagePool&) = delete;

        PagePool(PagePool&& o) noexcept : chunks_(std::move(o.chunks_)), size_(o.size_) {
            o.size_ = 0;
        }

        PagePool& operator=(PagePool&& o) noexcept {
            if (this != &o) {
                for (auto* chunk : chunks_) {
                    ::munmap(chunk, sizeof(AdjacencyPage) * CHUNK_SIZE);
                }
                chunks_ = std::move(o.chunks_);
                size_ = o.size_;
                o.size_ = 0;
            }
            return *this;
        }

        uint32_t allocate_page() {
            uint32_t id = static_cast<uint32_t>(size_);
            size_t chunk_idx = id / CHUNK_SIZE;
            if (chunk_idx >= chunks_.size()) {
                void* ptr = ::mmap(nullptr, sizeof(AdjacencyPage) * CHUNK_SIZE,
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (ptr == MAP_FAILED) {
                    throw std::bad_alloc();
                }
#ifdef MADV_HUGEPAGE
                ::madvise(ptr, sizeof(AdjacencyPage) * CHUNK_SIZE, MADV_HUGEPAGE);
#endif
                chunks_.push_back(static_cast<AdjacencyPage*>(ptr));
            }
            size_++;
            auto& page = chunks_[chunk_idx][id % CHUNK_SIZE];
            uint64_t* h = reinterpret_cast<uint64_t*>(&page);
            h[0] = 0;
            h[1] = 0;
            return id;
        }

        void emplace_back() {
            allocate_page();
        }

        [[nodiscard]] size_t size() const noexcept {
            return size_;
        }

        [[nodiscard]] AdjacencyPage& operator[](size_t index) noexcept {
            return chunks_[index / CHUNK_SIZE][index % CHUNK_SIZE];
        }

        [[nodiscard]] const AdjacencyPage& operator[](size_t index) const noexcept {
            return chunks_[index / CHUNK_SIZE][index % CHUNK_SIZE];
        }

        [[nodiscard]] AdjacencyPage& back() noexcept {
            return (*this)[size_ - 1];
        }

        [[nodiscard]] const AdjacencyPage& back() const noexcept {
            return (*this)[size_ - 1];
        }

    private:
        std::vector<AdjacencyPage*> chunks_;
        size_t size_{0};
    };

    class FlatVertexMap {
    public:
        static constexpr uint64_t kEmptyKey = std::numeric_limits<uint64_t>::max();

        struct Entry {
            uint64_t first{kEmptyKey};
            VertexRecord second;
        };

        class const_iterator {
        public:
            const_iterator(const Entry* cur, const Entry* end_ptr, const Entry* sentinel) noexcept
                : cur_(cur), end_(end_ptr), sentinel_(sentinel) {
                advance_to_occupied();
            }
            const Entry& operator*() const noexcept { return cur_ ? *cur_ : *sentinel_; }
            const Entry* operator->() const noexcept { return cur_ ? cur_ : sentinel_; }
            const_iterator& operator++() noexcept {
                if (cur_) {
                    ++cur_;
                    advance_to_occupied();
                } else {
                    sentinel_ = nullptr;
                }
                return *this;
            }
            bool operator==(const const_iterator& o) const noexcept {
                return cur_ == o.cur_ && sentinel_ == o.sentinel_;
            }
            bool operator!=(const const_iterator& o) const noexcept { return !(*this == o); }
            friend bool operator==(const Entry* p, const const_iterator& o) noexcept {
                return p == o.cur_ && o.sentinel_ == nullptr;
            }

        private:
            void advance_to_occupied() noexcept {
                while (cur_ < end_ && cur_->first == kEmptyKey) {
                    ++cur_;
                }
                if (cur_ >= end_) {
                    cur_ = nullptr;
                }
            }
            const Entry* cur_{nullptr};
            const Entry* end_{nullptr};
            const Entry* sentinel_{nullptr};
        };

        FlatVertexMap() { rehash(64); }

        void reserve(size_t n) {
            size_t needed = n * 2;
            if (needed > slots_.size()) {
                size_t cap = 64;
                while (cap < needed) cap <<= 1;
                rehash(cap);
            }
        }

        [[nodiscard]] const Entry* find(uint64_t key) const noexcept {
            if (key == kEmptyKey) {
                return has_sentinel_ ? &sentinel_ : nullptr;
            }
            const size_t mask = slots_.size() - 1;
            size_t idx = hash_key(key) & mask;
            for (;;) {
                const Entry& e = slots_[idx];
                if (e.first == key) return &e;
                if (e.first == kEmptyKey) return nullptr;
                idx = (idx + 1) & mask;
            }
        }

        [[nodiscard]] Entry* find(uint64_t key) noexcept {
            return const_cast<Entry*>(static_cast<const FlatVertexMap*>(this)->find(key));
        }

        VertexRecord& operator[](uint64_t key) {
            if (key == kEmptyKey) {
                if (!has_sentinel_) {
                    sentinel_.first = kEmptyKey;
                    sentinel_.second = VertexRecord();
                    has_sentinel_ = true;
                }
                return sentinel_.second;
            }
            if ((size_ + 1) * 2 > slots_.size()) {
                rehash(slots_.size() * 2);
            }
            const size_t mask = slots_.size() - 1;
            size_t idx = hash_key(key) & mask;
            for (;;) {
                Entry& e = slots_[idx];
                if (e.first == key) {
                    return e.second;
                }
                if (e.first == kEmptyKey) {
                    e.first = key;
                    e.second = VertexRecord();
                    ++size_;
                    return e.second;
                }
                idx = (idx + 1) & mask;
            }
        }

        [[nodiscard]] const_iterator begin() const noexcept {
            return const_iterator(slots_.data(), slots_.data() + slots_.size(),
                                  has_sentinel_ ? &sentinel_ : nullptr);
        }
        [[nodiscard]] const_iterator end() const noexcept {
            return const_iterator(nullptr, nullptr, nullptr);
        }

    private:
        static size_t hash_key(uint64_t x) noexcept {
            x ^= x >> 30;
            x *= 0xbf58476d1ce4e5b9ULL;
            x ^= x >> 27;
            x *= 0x94d049bb133111ebULL;
            x ^= x >> 31;
            return static_cast<size_t>(x);
        }

        void rehash(size_t new_cap) {
            std::vector<Entry> next(new_cap);
            const size_t mask = new_cap - 1;
            for (const Entry& old : slots_) {
                if (old.first == kEmptyKey) continue;
                size_t idx = hash_key(old.first) & mask;
                while (next[idx].first != kEmptyKey) {
                    idx = (idx + 1) & mask;
                }
                next[idx] = old;
            }
            slots_.swap(next);
        }

        std::vector<Entry> slots_;
        size_t size_{0};
        bool has_sentinel_{false};
        Entry sentinel_{};
    };

    struct alignas(64) Shard {
        mutable SharedSpinLock mutex;
        FlatVertexMap directory;
        PagePool page_pool;

        Shard() {
            directory.reserve(4096);
        }
    };

    std::array<Shard, NUM_SHARDS> shards_;
};

} // namespace l3kvg
