#pragma once

#include "L3KVG/Engine.hpp"
#include "L3KVG/Query.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace l3kvg {

class CypherParser {
public:
  explicit CypherParser(Engine *engine);

  // Parses a Cypher subset query and triggers execution, returning rows.
  std::vector<Query::ResultRow> execute(std::string_view query_text);

private:
  Engine *engine_;
};

} // namespace l3kvg
