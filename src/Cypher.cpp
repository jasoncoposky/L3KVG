#include "L3KVG/Cypher.hpp"
#include <cctype>
#include <regex>
#include <stdexcept>

namespace l3kvg {

CypherParser::CypherParser(Engine *engine) : engine_(engine) {}

// A very fast, lightweight regex-assisted parser specifically built to parse
// OpenCypher Subsets mapped identically to the L3KVG Fluent Traversal API
// capabilities.
std::vector<Query::ResultRow>
CypherParser::execute(std::string_view query_text) {
  std::string query(query_text);
  Query q = engine_->query();

  // We utilize a simple multi-pass regex extractor to build the AST since we
  // are enforcing a strict syntax subset for the MVP Embedded Cypher Parser.

  // MATCH (n {id: 'npc_1', type: 'npc'})-[e:loyalty]->(ally {faction: 'stark'})
  // WHERE e.weight >= 0.5
  // RETURN ally.name

  std::regex match_rx(
      R"(MATCH\s*\(([^ {}]+)(?:\s*\{([^}]+)\})?\)\s*-\[\s*([^ :]+)\s*:\s*([^ \]]+)\s*\]->\s*\(([^ {}]+)(?:\s*\{([^}]+)\})?\))");
  std::smatch match_ast;

  if (!std::regex_search(query, match_ast, match_rx)) {
    throw std::runtime_error(
        "Cypher Syntax Error: Invalid MATCH clause layout.");
  }

  std::string src_alias = match_ast[1];
  std::string src_props = match_ast[2];
  std::string edge_alias = match_ast[3];
  std::string edge_label = match_ast[4];
  std::string tgt_alias = match_ast[5];
  std::string tgt_props = match_ast[6];

  q.match(src_alias);

  // Helper to parse `{key: 'value', key2: 'value2'}`
  auto parse_props = [&](const std::string &props, const std::string &alias) {
    if (props.empty())
      return;
    std::regex extract_rx(R"(([^:, ']+)\s*:\s*'([^']+)')");
    auto words_begin =
        std::sregex_iterator(props.begin(), props.end(), extract_rx);
    auto words_end = std::sregex_iterator();
    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
      std::smatch match = *i;
      q.where_eq(alias, match[1].str(), match[2].str());
    }
  };

  parse_props(src_props, src_alias);

  // Search for WHERE clause
  double min_weight = -999999.0;
  std::regex where_rx(R"(WHERE\s+([^\.]+)\.([^ ]+)\s*(>=|>|=)\s*([0-9\.]+))");
  std::smatch where_ast;
  if (std::regex_search(query, where_ast, where_rx)) {
    std::string w_alias = where_ast[1];
    std::string w_prop = where_ast[2];
    std::string w_num = where_ast[4];

    if (w_alias == edge_alias && w_prop == "weight") {
      min_weight = std::stod(w_num);
    }
  }

  q.out(edge_label, min_weight).as(tgt_alias);
  parse_props(tgt_props, tgt_alias);

  // Search for RETURN clause
  std::regex return_rx(R"(RETURN\s+([^\.]+)\.([^ \n\r]+))");
  std::smatch return_ast;
  if (!std::regex_search(query, return_ast, return_rx)) {
    throw std::runtime_error("Cypher Syntax Error: Missing RETURN clause.");
  }

  std::string ret_alias = return_ast[1];
  std::string ret_prop = return_ast[2];
  q.return_(ret_alias, ret_prop);

  return q.execute();
}

} // namespace l3kvg
