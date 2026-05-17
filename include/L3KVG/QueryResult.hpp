#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

namespace l3kvg {

class Node;

struct ResultRow {
    std::unordered_map<std::string, std::string> fields;
    std::vector<std::shared_ptr<Node>> nodes; // Keep memory alive for views
};

} // namespace l3kvg
