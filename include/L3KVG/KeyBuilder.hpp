#pragma once

#include <string_view>
#include <string>
#include <cstring>
#include <cstdio>
#include <array>

namespace l3kvg {

class KeyBuilder {
public:
    static constexpr size_t MAX_KEY_SIZE = 1024;

    // Build node key: n:{uuid}
    static std::string_view node_key(std::string_view uuid) {
        char* buf = get_buffer();
        int len = std::snprintf(buf, MAX_KEY_SIZE, "n:%.*s", 
                                static_cast<int>(uuid.size()), uuid.data());
        if (len < 0 || len >= static_cast<int>(MAX_KEY_SIZE)) return {};
        return std::string_view(buf, static_cast<size_t>(len));
    }

    // Build edge out prefix: e:out:{src}:{label}:
    static std::string_view edge_prefix(std::string_view src_uuid, std::string_view label) {
        char* buf = get_buffer();
        int len = std::snprintf(buf, MAX_KEY_SIZE, "e:out:%.*s:%.*s:",
                                static_cast<int>(src_uuid.size()), src_uuid.data(),
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
    static std::string_view edge_out_key(std::string_view src_uuid, std::string_view label, 
                                         double weight, std::string_view dst_uuid) {
        char* buf = get_buffer();
        int len = std::snprintf(buf, MAX_KEY_SIZE, "e:out:%.*s:%.*s:%012.4f:%.*s", 
                                static_cast<int>(src_uuid.size()), src_uuid.data(), 
                                static_cast<int>(label.size()), label.data(),
                                weight,
                                static_cast<int>(dst_uuid.size()), dst_uuid.data());
        if (len < 0 || len >= static_cast<int>(MAX_KEY_SIZE)) return {};
        return std::string_view(buf, static_cast<size_t>(len));
    }

    // Build incoming edge key: e:in:{dst}:{label}:{src}
    static std::string_view edge_in_key(std::string_view dst_uuid, std::string_view label, 
                                        std::string_view src_uuid) {
        char* buf = get_buffer();
        int len = std::snprintf(buf, MAX_KEY_SIZE, "e:in:%.*s:%.*s:%.*s", 
                                static_cast<int>(dst_uuid.size()), dst_uuid.data(), 
                                static_cast<int>(label.size()), label.data(),
                                static_cast<int>(src_uuid.size()), src_uuid.data());
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
