#include "L3KVG/SIMDFilter.hpp"

namespace l3kvg {

void SIMDFilter::filter_edges(std::span<const PackedEdge> edges,
                             uint16_t target_label,
                             float min_weight,
                             std::vector<uint64_t>& matched_targets) {
    filter_edges_cb(edges, target_label, min_weight, [&](const PackedEdge& edge) {
        matched_targets.push_back(edge.target_id);
    });
}

size_t SIMDFilter::count_matching_edges(std::span<const PackedEdge> edges,
                                        uint16_t target_label,
                                        float min_weight) noexcept {
    const size_t n = edges.size();
    if (n == 0) return 0;

    const bool is_wildcard = (target_label == 0);
    size_t count = 0;
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

        __m256i lbl0, lbl1;
        if (is_wildcard) {
            lbl0 = _mm256_set1_epi32(-1);
            lbl1 = _mm256_set1_epi32(-1);
        } else {
            lbl0 = _mm256_cmpeq_epi32(_mm256_and_si256(lf0, lbl_mask), target_vec);
            lbl1 = _mm256_cmpeq_epi32(_mm256_and_si256(lf1, lbl_mask), target_vec);
        }

        __m256 wt_match0 = _mm256_cmp_ps(wt0, min_wt_vec, _CMP_GE_OQ);
        __m256 wt_match1 = _mm256_cmp_ps(wt1, min_wt_vec, _CMP_GE_OQ);

        __m256i res0 = _mm256_and_si256(_mm256_and_si256(is_act0, lbl0), _mm256_castps_si256(wt_match0));
        __m256i res1 = _mm256_and_si256(_mm256_and_si256(is_act1, lbl1), _mm256_castps_si256(wt_match1));

        int m0 = _mm256_movemask_ps(_mm256_castsi256_ps(res0));
        int m1 = _mm256_movemask_ps(_mm256_castsi256_ps(res1));

        count += std::popcount(static_cast<uint32_t>((m1 << 8) | m0));
    }

    for (; i + 8 <= n; i += 8) {
        const int* base = reinterpret_cast<const int*>(&edges[i]);
        __m256i lf = _mm256_i32gather_epi32(base, vindex_lf, 4);
        __m256  wt = _mm256_i32gather_ps(reinterpret_cast<const float*>(base), vindex_wt, 4);

        __m256i is_act = _mm256_cmpeq_epi32(_mm256_and_si256(lf, act_mask), act_mask);
        __m256i lbl_match;
        if (is_wildcard) {
            lbl_match = _mm256_set1_epi32(-1);
        } else {
            __m256i edge_lbls = _mm256_and_si256(lf, lbl_mask);
            lbl_match = _mm256_cmpeq_epi32(edge_lbls, target_vec);
        }
        __m256 wt_match = _mm256_cmp_ps(wt, min_wt_vec, _CMP_GE_OQ);

        __m256i res = _mm256_and_si256(is_act, lbl_match);
        res = _mm256_and_si256(res, _mm256_castps_si256(wt_match));

        int mask = _mm256_movemask_ps(_mm256_castsi256_ps(res));
        count += std::popcount(static_cast<uint32_t>(mask));
    }
#endif

    // Branchless scalar fallback
    for (; i < n; ++i) {
        const auto& edge = edges[i];
        const bool is_active = edge.is_active();
        const bool label_match = is_wildcard | (edge.label_id == target_label);
        const bool weight_match = (edge.weight >= min_weight);
        count += static_cast<size_t>(is_active & label_match & weight_match);
    }

    return count;
}

} // namespace l3kvg
