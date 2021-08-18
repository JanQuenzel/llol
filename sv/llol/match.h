#pragma once

#include <opencv2/core/mat.hpp>

#include "sv/llol/grid.h"
#include "sv/llol/pano.h"
#include "sv/util/math.h"  // MeanCovar

namespace sv {

/// @struct Match
struct PointMatch {
  static constexpr int kBad = -100;

  cv::Point px_s{kBad, kBad};  // 8
  MeanCovar3f mc_s{};          // 52 sweep
  cv::Point px_p{kBad, kBad};  // 8
  MeanCovar3f mc_p{};          // 52 pano
  Eigen::Matrix3f U{};

  /// @brief Whether this match is good
  bool ok() const noexcept {
    return px_s.x >= 0 && px_p.x >= 0 && mc_s.ok() && mc_p.ok();
  }

  void SqrtInfo(float lambda = 0.0F);
};

/// @class Feature Matcher
struct MatcherParams {
  int half_rows{2};        // half rows of pano win
  float min_dist{2.0};     // min dist for recompute mc in pano
  float cov_lambda{1e-6};  // lambda added to diagonal of cov when inverting

  std::string Repr() const;
  friend std::ostream& operator<<(std::ostream& os, const MatcherParams& rhs) {
    return os << rhs.Repr();
  }
};

struct ProjMatcher {
  /// Data
  int min_pts;             // min pts in pano win for a valid match
  cv::Size grid_size;      // copy of grid_size from SweepGrid
  cv::Size pano_win_size;  // win size in pano used to compute mean covar
  MatcherParams params;
  std::vector<PointMatch> matches;

  /// @brief Ctors
  ProjMatcher() = default;
  ProjMatcher(const cv::Size& grid_size, const MatcherParams& params);

  std::string Repr() const;
  friend std::ostream& operator<<(std::ostream& os, const ProjMatcher& rhs) {
    return os << rhs.Repr();
  }

  /// @brief Match features in sweep to pano using mask
  /// @return Number of final matches
  int Match(const LidarSweep& sweep,
            const SweepGrid& grid,
            const DepthPano& pano,
            bool tbb = false);
  int MatchRow(const LidarSweep& sweep,
               const SweepGrid& grid,
               const DepthPano& pano,
               int gr);
  int MatchCell(const LidarSweep& sweep,
                const SweepGrid& grid,
                const DepthPano& pano,
                const cv::Point& px_g);

  void Reset();
};

/// @brief Draw match, valid pixel is percentage of pano points in window
cv::Mat DrawMatches(const SweepGrid& grid,
                    const std::vector<PointMatch>& matches);

}  // namespace sv
