#pragma once

#include <string_view>
#include <string>
#include <cstring>
#include <cstdio>
#include <array>
#include <cstdint>

namespace l3kvg {

class KeyBuilder {
public:
    static constexpr size_t MAX_KEY_SIZE = 1024;

    // Build node key: n:{id}
    static std::string_view node_key(uint64_t id) {
        char* buf = get_buffer();
        int len = std::snprintf(buf, MAX_KEY_SIZE, "n:{%016llx\x7d", 
                                static_cast<unsigned long long>(id));
        if (len < 0 || len >= static_cast<int>(MAX_KEY_SIZE)) return {};
        return std::string_view(buf, static_cast<size_t>(len));
    }

    // Build edge out prefix: e:out:{src}:{label}:
    static std::string_view edge_prefix(uint64_t src_id, std::string_view label) {
        char* buf = get_buffer();
        int len = std::snprintf(buf, MAX_KEY_SIZE, "e:out:{%016llx\x7d:%.*s:",
                                static_cast<unsigned long long>(src_id),
                                static_cast<int>(label.size()), label.data());
        if (len < 0 || len >= static_cast<int>(MAX_KEY_SIZE)) return {};
        return std::string_view(buf, static_cast<size_t>(len));
    }

    // Format weight: 00000000.0000
    static std::string_view format_weight(double weight) {
        char* buf = get_buffer();
        int len = std::snprintf(buf, MAX_KEY_SIZE, "%012.4f", weight);
        if (len < 0 || len >= static_cast<int>(MAX_KEY_SIZE)) return {};
        return std::string_view(buf, static_cast<size_t>(len));
    }

    // Build edge key: e:out:{src}:{label}:{weight}:{dst}
    static std::string_view edge_out_key(uint64_t src_id, std::string_view label, 
                                         double weight, uint64_t dst_id) {
        char* buf = get_buffer();
        int len = std::snprintf(buf, MAX_KEY_SIZE, "e:out:{%016llx\x7d:%.*s:%012.4f:{%016llx\x7d", 
                                static_cast<unsigned long long>(src_id),
                                static_cast<int>(label.size()), label.data(),
                                weight,
                                static_cast<unsigned long long>(dst_id));
        if (len < 0 || len >= static_cast<int>(MAX_KEY_SIZE)) return {};
        return std::string_view(buf, static_cast<size_t>(len));
    }

    // Build incoming edge prefix: e:in:{dst}:{label}:
    static std::string_view edge_in_prefix(uint64_t dst_id, std::string_view label) {
        char* buf = get_buffer();
        int len = std::snprintf(buf, MAX_KEY_SIZE, "e:in:{%016llx\x7d:%.*s:",
                                static_cast<unsigned long long>(dst_id),
                                static_cast<int>(label.size()), label.data());
        if (len < 0 || len >= static_cast<int>(MAX_KEY_SIZE)) return {};
        return std::string_view(buf, static_cast<size_t>(len));
    }

    // Build incoming edge key: e:in:{dst}:{label}:{src}
    static std::string_view edge_in_key(uint64_t dst_id, std::string_view label, 
                                         uint64_t src_id) {
        char* buf = get_buffer();
        int len = std::snprintf(buf, MAX_KEY_SIZE, "e:in:{%016llx\x7d:%.*s:{%016llx\x7d", 
                                static_cast<unsigned long long>(dst_id),
                                static_cast<int>(label.size()), label.data(),
                                static_cast<unsigned long long>(src_id));
        if (len < 0 || len >= static_cast<int>(MAX_KEY_SIZE)) return {};
        return std::string_view(buf, static_cast<size_t>(len));
    }

private:
    static char* get_buffer() {
        static thread_local std::array<std::array<char, MAX_KEY_SIZE>, 2> buffers;
        static thread_local size_t index = 0;
        char* buf = buffers[index].data();
        index = (index + 1) % 2;
        return buf;
    }
};

} // namespace l3kvg
