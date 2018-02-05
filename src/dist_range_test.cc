#include "dist_range.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "concurrent_map_impl.h"
#include "reducer.h"

TEST(DistRangeTest, MapReduceTest) {
  const int N_KEYS = 1000;
  hpmr::DistRange<int> range(0, N_KEYS);
  const auto& mapper = [](const int id, const std::function<void(const int, const bool)>& emit) {
    EXPECT_THAT(id, testing::Ge(0));
    EXPECT_THAT(id, testing::Lt(N_KEYS));
    emit(id, false);
  };
  auto dist_map = range.mapreduce<int, bool>(mapper, hpmr::Reducer<bool>::keep);
  EXPECT_EQ(dist_map.get_n_keys(), N_KEYS);
}
