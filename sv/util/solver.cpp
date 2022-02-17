#include "sv/util/solver.h"

//#define FMT_HEADER_ONLY
#include <fmt/core.h>

namespace sv {

std::string Repr(SolverStatus status) {
  switch (status) {
    case SolverStatus::COST_TOO_SMALL:
      return "COST_TOO_SMALL";
    case SolverStatus::GRADIENT_TOO_SMALL:
      return "GRAD_TOO_SMALL";
    case SolverStatus::RELATIVE_STEP_SIZE_TOO_SMALL:
      return "REL_STEP_SIZE_TOO_SMALL";
    case SolverStatus::HIT_MAX_ITERATIONS:
      return "HIT_MAX_ITERS";
    default:
      return "UNKNOWN";
  }
}

std::string SolverSummary::Report() const {
  return fmt::format(
      "init_cost={:.6e}, final_cost={:.6e}, grad_max_norm={:.6e}, iters={}, "
      "status={}",
      initial_cost,
      final_cost,
      gradient_max_norm,
      iterations,
      Repr(status));
//  return (static_cast<std::stringstream&>(std::stringstream()
//          << "init_cost=" << initial_cost
//          << ", final_cost=" << final_cost
//          << ", grad_max_norm=" << gradient_max_norm
//          << ", iters=" << iterations
//          << ", status=" << Repr(status))).str();
}

bool SolverSummary::IsConverged() const {
  return status == SolverStatus::GRADIENT_TOO_SMALL ||
         status == SolverStatus::RELATIVE_STEP_SIZE_TOO_SMALL ||
         status == SolverStatus::COST_TOO_SMALL;
}

}  // namespace sv
