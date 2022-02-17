#include "sv/util/manager.h"

//#define FMT_HEADER_ONLY
#include <fmt/color.h>
#include <fmt/ostream.h>
#include <glog/logging.h>

namespace sv {

std::string StatsManager::ReportStats(const std::string& name,
                                      const StatsT& stats) const {
  std::string str = fmt::format(fmt::fg(fmt::color::cyan), "[{:<16}]", name);
//      "[" + name + "]";
  str += fmt::format(
      " n: {:<16} | sum: {:<14.4e} | min: {:<14.4f} | max: {:<14.4f} | "
      "mean: {:<14.4f} | last: {:<14.4f} |",
      stats.count(),
      stats.sum(),
      stats.min(),
      stats.max(),
      stats.mean(),
      stats.last());
  return str;
//  str += (static_cast<std::stringstream&>(std::stringstream()
//          << " n=" << stats.count()
//          << ", sum=" << stats.sum()
//          << ", min=" << stats.min()
//          << ", max=" << stats.max()
//          << ", mean=" << stats.mean()
//          << ", last=" << stats.last())).str();
//  return str;
}

StatsManager& GlobalStatsManager() {
  static StatsManager sm{};
  return sm;
}

TimerManager::ManualTimer::ManualTimer(std::string name,
                                       TimerManager* manager,
                                       bool start)
    : name_{std::move(name)}, manager_{manager} {
  CHECK_NOTNULL(manager_);
  if (start) {
    timer_.Start();
  } else {
    timer_.Reset();
  }
}

void TimerManager::ManualTimer::Stop(bool record) {
  timer_.Stop();
  if (record) {
    stats_.Add(absl::Nanoseconds(timer_.Elapsed()));
  }
}

void TimerManager::ManualTimer::Commit() {
  Stop(true);
  if (!stats_.ok()) return;  // Noop if there's no stats to commit

  // Already checked in ctor
  // CHECK_NOTNULL(manager_);
  manager_->Update(name_, stats_);
  stats_ = StatsT{};  // reset stats
}

std::string TimerManager::ReportStats(const std::string& name,
                                      const StatsT& stats) const {
  std::string str = fmt::format(fmt::fg(fmt::color::light_sky_blue), "[{:<16}]", name);
//      "[" + name + "]";
  str += fmt::format(
      " n: {:<16} | sum: {:<14} | min: {:<14} | max: {:<14} | mean: {:<14} | "
      "last: {:<14} |",
      stats.count(),
      stats.sum(),
      stats.min(),
      stats.max(),
      stats.mean(),
      stats.last());

//  str += (static_cast<std::stringstream&>(std::stringstream()
//          << " n=" << stats.count()
//          << ", sum=" << stats.sum()
//          << ", min=" << stats.min()
//          << ", max=" << stats.max()
//          << ", mean=" << stats.mean()
//          << ", last=" << stats.last())).str();
  return str;
}

TimerManager& GlobalTimerManager() {
  static TimerManager tm{};
  return tm;
}

}  // namespace sv
