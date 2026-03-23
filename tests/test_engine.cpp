#include "L3KVG/Engine.hpp"
#include <gtest/gtest.h>
#include <string>


TEST(EngineTest, FormatWeight) {
  std::string w1 = l3kvg::Engine::format_weight(0.85);
  std::string w2 = l3kvg::Engine::format_weight(1.0);
  std::string w3 = l3kvg::Engine::format_weight(0.05);

  EXPECT_EQ(w1, "000.8500");
  EXPECT_EQ(w2, "001.0000");
  EXPECT_EQ(w3, "000.0500");

  // Lexicographical sorting check
  EXPECT_TRUE(w3 < w1);
  EXPECT_TRUE(w1 < w2);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
