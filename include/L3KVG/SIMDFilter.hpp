#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
#include <type_traits>
#include <bit>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include "L3KVG/PackedEdge.hpp"

namespace l3kvg {

static_assert(offsetof(PackedEdge, label_id) == 8, "PackedEdge::label_id offset mismatch for SIMD gather");
static_assert(offsetof(PackedEdge, flags) == 10, "PackedEdge::flags offset mismatch for SIMD gather");
static_assert(offsetof(PackedEdge, weight) == 12, "PackedEdge::weight offset mismatch for SIMD gather");
static_assert(std::endian::native == std::endian::little, "SIMDFilter requires little-endian architecture for dword gather");


/**
 * @brief SIMD Predicate Filter Engine for high-throughput edge filtering.
 *
 * Provides vectorized (AVX2 on x86_64) and branchless scalar filtering of PackedEdge spans.
 * Correctness guarantees:
 * - Inactive edges (!edge.is_active()) are treated as tombstones and skipped.
 * - Wildcard matching: target_label == 0 matches all active edges with weight >= min_weight.
 * - Safe for empty spans, non-multiple of 8/16 spans, and arbitrarily large spans without buffer overreads.
 */
class SIMDFilter {
public:
    /**
     * @brief Filter edges matching target_label and min_weight, appending matched target_ids.
     *
     * @param edges The span of PackedEdge structures to evaluate.
     * @param target_label Encoded relationship label (0 = wildcard, matching any label).
     * @param min_weight Minimum edge weight threshold (inclusive: weight >= min_weight).
     * @param matched_targets Output vector where matched target_ids are appended.
     */
    static void filter_edges(std::span<const PackedEdge> edges,
                             uint16_t target_label,
                             float min_weight,
                             std::vector<uint64_t>& matched_targets);

    /**
     * @brief Counts matching edges without allocating or materializing target IDs.
     *
     * @param edges The span of PackedEdge structures to evaluate.
     * @param target_label Encoded relationship label (0 = wildcard, matching any label).
     * @param min_weight Minimum edge weight threshold (inclusive: weight >= min_weight).
     * @return Number of edges matching the criteria.
     */
    static size_t count_matching_edges(std::span<const PackedEdge> edges,
                                       uint16_t target_label,
                                       float min_weight) noexcept;

    /**
     * @brief High-performance callback visitor invoking cb on each matching edge.
     *
     * Supports callbacks taking either `const PackedEdge&` or `uint64_t` (target_id).
     *
     * @tparam F Callback invocable with (const PackedEdge&) or (uint64_t).
     * @param edges The span of PackedEdge structures to evaluate.
     * @param target_label Encoded relationship label (0 = wildcard).
     * @param min_weight Minimum edge weight threshold.
     * @param cb Callback function.
     */
    template <typename F>
    static void filter_edges_cb(std::span<const PackedEdge> edges,
                                uint16_t target_label,
                                float min_weight,
                                F&& cb) {
        if (target_label == 0) {
            filter_edges_cb_impl<true>(edges, 0, min_weight, std::forward<F>(cb));
        } else {
            filter_edges_cb_impl<false>(edges, target_label, min_weight, std::forward<F>(cb));
        }
    }

private:
    template <bool IsWildcard, typename F>
    static void filter_edges_cb_impl(std::span<const PackedEdge> edges,
                                     uint16_t target_label,
                                     float min_weight,
                                     F&& cb) {
        const size_t n = edges.size();
        if (n == 0) return;

        size_t i = 0;

#if defined(__AVX2__)
        const __m256i vindex_lf = _mm256_setr_epi32(2, 8, 14, 20, 26, 32, 38, 44);
        const __m256i vindex_wt = _mm256_setr_epi32(3, 9, 15, 21, 27, 33, 39, 45);
        const __m256 min_wt_vec = _mm256_set1_ps(min_weight);
        const __m256i act_mask = _mm256_set1_epi32(static_cast<int>(PackedEdge::FLAG_ACTIVE << 16));
        const __m256i lbl_mask = _mm256_set1_epi32(0x0000FFFF);
        const __m256i target_vec = _mm256_set1_epi32(target_label);

        for (; i + 16 <= n; i += 16) {
            const int* b0 = reinterpret_cast<const int*>(&edges[i]);
            const int* b1 = reinterpret_cast<const int*>(&edges[i + 8]);

            __m256i lf0 = _mm256_i32gather_epi32(b0, vindex_lf, 4);
            __m256  wt0 = _mm256_i32gather_ps(reinterpret_cast<const float*>(b0), vindex_wt, 4);

            __m256i lf1 = _mm256_i32gather_epi32(b1, vindex_lf, 4);
            __m256  wt1 = _mm256_i32gather_ps(reinterpret_cast<const float*>(b1), vindex_wt, 4);

            __m256i is_act0 = _mm256_cmpeq_epi32(_mm256_and_si256(lf0, act_mask), act_mask);
            __m256i is_act1 = _mm256_cmpeq_epi32(_mm256_and_si256(lf1, act_mask), act_mask);

            __m256 wt_match0 = _mm256_cmp_ps(wt0, min_wt_vec, _CMP_GE_OQ);
            __m256 wt_match1 = _mm256_cmp_ps(wt1, min_wt_vec, _CMP_GE_OQ);

            __m256i res0 = _mm256_and_si256(is_act0, _mm256_castps_si256(wt_match0));
            __m256i res1 = _mm256_and_si256(is_act1, _mm256_castps_si256(wt_match1));

            if constexpr (!IsWildcard) {
                __m256i lbl0 = _mm256_cmpeq_epi32(_mm256_and_si256(lf0, lbl_mask), target_vec);
                __m256i lbl1 = _mm256_cmpeq_epi32(_mm256_and_si256(lf1, lbl_mask), target_vec);
                res0 = _mm256_and_si256(res0, lbl0);
                res1 = _mm256_and_si256(res1, lbl1);
            }

            uint32_t mask = static_cast<uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(res0))) |
                            (static_cast<uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(res1))) << 8);
            while (mask != 0) {
                int bit = std::countr_zero(mask);
                invoke_cb(cb, edges[i + bit]);
                mask &= (mask - 1);
            }
        }

        for (; i + 8 <= n; i += 8) {
            const int* base = reinterpret_cast<const int*>(&edges[i]);
            __m256i lf = _mm256_i32gather_epi32(base, vindex_lf, 4);
            __m256  wt = _mm256_i32gather_ps(reinterpret_cast<const float*>(base), vindex_wt, 4);

            __m256i is_act = _mm256_cmpeq_epi32(_mm256_and_si256(lf, act_mask), act_mask);
            __m256 wt_match = _mm256_cmp_ps(wt, min_wt_vec, _CMP_GE_OQ);
            __m256i res = _mm256_and_si256(is_act, _mm256_castps_si256(wt_match));

            if constexpr (!IsWildcard) {
                __m256i lbl_match = _mm256_cmpeq_epi32(_mm256_and_si256(lf, lbl_mask), target_vec);
                res = _mm256_and_si256(res, lbl_match);
            }

            uint32_t mask = static_cast<uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(res)));
            while (mask != 0) {
                int bit = std::countr_zero(mask);
                invoke_cb(cb, edges[i + bit]);
                mask &= (mask - 1);
            }
        }
#endif

        // Scalar fallback / tail loop with branchless evaluation
        for (; i < n; ++i) {
            const auto& edge = edges[i];
            const bool is_active = edge.is_active();
            const bool weight_match = (edge.weight >= min_weight);
            if constexpr (IsWildcard) {
                if (is_active & weight_match) {
                    invoke_cb(cb, edge);
                }
            } else {
                const bool label_match = (edge.label_id == target_label);
                if (is_active & label_match & weight_match) {
                    invoke_cb(cb, edge);
                }
            }
        }
    }
    template <typename F>
    static inline void invoke_cb(F&& cb, const PackedEdge& edge) {
        if constexpr (std::is_invocable_v<F, const PackedEdge&>) {
            cb(edge);
        } else if constexpr (std::is_invocable_v<F, uint64_t>) {
            cb(edge.target_id);
        } else {
            static_assert(std::is_invocable_v<F, const PackedEdge&> || std::is_invocable_v<F, uint64_t>,
                "SIMDFilter::filter_edges_cb callback must accept (const PackedEdge&) or (uint64_t)");
        }
    }
};

} // namespace l3kvg
