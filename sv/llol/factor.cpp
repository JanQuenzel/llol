#include "sv/llol/factor.h"

#include <glog/logging.h>

namespace sv {

using SE3d = Sophus::SE3d;

bool LocalParamSE3::Plus(const double* _T,
                         const double* _x,
                         double* _T_plus_x) const {
  Eigen::Map<const SE3d> T(_T);
  Eigen::Map<const Eigen::Matrix<double, 6, 1>> t(_x);
  Eigen::Map<SE3d> T_plus_del(_T_plus_x);
  T_plus_del = T * SE3d::exp(t);
  return true;
}

bool LocalParamSE3::ComputeJacobian(const double* _T, double* _J) const {
  Eigen::Map<SE3d const> T(_T);
  Eigen::Map<
      Eigen::Matrix<double, SE3d::num_parameters, SE3d::DoF, Eigen::RowMajor>>
      J(_J);
  J = T.Dx_this_mul_exp_x_at_0();
  return true;
}

GicpFactor::GicpFactor(const PointMatch& match) {
  pt_s = match.mc_s.mean.cast<double>();
  pt_p = match.mc_p.mean.cast<double>();
  U = match.U.cast<double>();
}

}  // namespace sv