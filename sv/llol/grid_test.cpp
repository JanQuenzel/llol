#include "sv/llol/grid.h"

#include <benchmark/benchmark.h>
#include <gtest/gtest.h>

namespace sv {
namespace {

TEST(GridTest, TestCtor) {
  const SweepGrid grid({1024, 64});
  EXPECT_EQ(grid.total(), 2048);
  EXPECT_EQ(grid.full(), false);
  EXPECT_EQ(grid.size().width, 64);
  EXPECT_EQ(grid.size().height, 32);
  EXPECT_EQ(grid.col_rg.start, 0);
  EXPECT_EQ(grid.col_rg.end, 0);
  EXPECT_EQ(grid.pano_win_size.height, 5);
  EXPECT_EQ(grid.pano_win_size.width, 9);
  std::cout << grid << std::endl;
}

TEST(GridTest, TestConversion) {
  const SweepGrid grid({1024, 64});
  EXPECT_EQ(grid.Sweep2Grid({0, 0}), cv::Point(0, 0));
  EXPECT_EQ(grid.Sweep2Grid({1, 1}), cv::Point(0, 0));
  EXPECT_EQ(grid.Grid2Sweep({0, 0}), cv::Point(0, 0));
  EXPECT_EQ(grid.Grid2Sweep({1, 1}), cv::Point(16, 2));
}

TEST(GridTest, TestScore) {
  SweepGrid grid({1024, 64});

  auto scan = MakeTestScan({512, 64});
  scan.col_rg = {0, 512};
  const auto n1 = grid.Score(scan);
  EXPECT_EQ(n1, 1024);
  EXPECT_EQ(grid.col_rg.end, 32);
  EXPECT_TRUE(std::isnan(grid.ScoreAt({32, 0})));
  EXPECT_EQ(grid.ScoreAt({0, 0}), 0);
  EXPECT_EQ(grid.ScoreAt({31, 0}), 0);
  std::cout << grid << std::endl;

  scan.col_rg = {512, 1024};
  const auto n2 = grid.Score(scan);
  EXPECT_EQ(n2, 1024);
  EXPECT_EQ(grid.col_rg.end, 64);
  std::cout << grid << std::endl;

  scan.col_rg = {0, 512};
  const auto n3 = grid.Score(scan);
  EXPECT_EQ(n3, 1024);
  EXPECT_EQ(grid.col_rg.end, 32);
  std::cout << grid << std::endl;
}

TEST(GridTest, TestMatch) {
  const auto scan = MakeTestScan({1024, 64});
  auto grid = SweepGrid(scan.size());
  grid.Add(scan);

  DepthPano pano({1024, 256});
  pano.dbuf.setTo(DepthPixel::kScale);

  const int n = grid.Match(pano);
  EXPECT_EQ(n, 1984);  // probably miss top and bottom
}

void BM_GridMatch(benchmark::State& state) {
  const auto scan = MakeTestScan({1024, 64});
  auto grid = SweepGrid(scan.size());
  grid.Add(scan);

  DepthPano pano({1024, 256});
  pano.dbuf.setTo(DepthPixel::kScale);

  const int gsize = state.range(0);
  for (auto _ : state) {
    grid.Match(pano, gsize);
  }
}
BENCHMARK(BM_GridMatch)->Arg(0)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

void BM_GridScore(benchmark::State& state) {
  const auto scan = MakeTestScan({1024, 64});
  SweepGrid grid(scan.size());
  const int gsize = state.range(0);

  for (auto _ : state) {
    auto n = grid.Score(scan, gsize);
    benchmark::DoNotOptimize(n);
  }
}
BENCHMARK(BM_GridScore)->Arg(0)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

void BM_GridFilter(benchmark::State& state) {
  const auto scan = MakeTestScan({1024, 64});
  SweepGrid grid(scan.size());
  grid.Score(scan);
  const int gsize = state.range(0);

  for (auto _ : state) {
    grid.Filter(scan, gsize);
    benchmark::DoNotOptimize(grid);
  }
}
BENCHMARK(BM_GridFilter)->Arg(0)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

void BM_GridInterpPoses(benchmark::State& state) {
  std::vector<Sophus::SE3f> poses_sweep(1024);
  std::vector<Sophus::SE3f> poses_grid(64 + 1);
  int gsize = state.range(0);

  for (auto _ : state) {
    InterpPosesImpl(poses_grid, 16, poses_sweep, gsize);
    benchmark::DoNotOptimize(poses_sweep);
  }
}
BENCHMARK(BM_GridInterpPoses)->Arg(0)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

}  // namespace
}  // namespace sv
