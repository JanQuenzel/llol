#include "sv/util/ocv.h"

//#include <fmt/full.h>
//#define FMT_HEADER_ONLY
#include <fmt/color.h>
#include <glog/logging.h>
#include <ostream>
#include <sstream>

namespace sv {

std::string CvTypeStr(int type) {
  cv::Mat a;
  std::string r;

  const uchar depth = type & CV_MAT_DEPTH_MASK;
  const uchar chans = static_cast<uchar>(1 + (type >> CV_CN_SHIFT));

  switch (depth) {
    case CV_8U:
      r = "8U";
      break;
    case CV_8S:
      r = "8S";
      break;
    case CV_16U:
      r = "16U";
      break;
    case CV_16S:
      r = "16S";
      break;
    case CV_32S:
      r = "32S";
      break;
    case CV_32F:
      r = "32F";
      break;
    case CV_64F:
      r = "64F";
      break;
    default:
      r = "User";
      break;
  }

  r += "C";
  r += (chans + '0');

  return r;
}

std::string Repr(const cv::Mat& mat) {
  return fmt::format("(hwc=({},{},{}), depth={})",
                     mat.rows,
                     mat.cols,
                     mat.channels(),
                     mat.depth());
//  return (static_cast<std::stringstream&>(std::stringstream()<< "(hwc=("<<mat.rows<< ", "<< mat.cols<< ","<< mat.channels()<< "), depth=" << mat.depth() << ")")).str();
}

std::string Repr(const cv::Size& size) {
  return fmt::format("(h={}, w={})", size.height, size.width);
//  return (static_cast<std::stringstream&>(std::stringstream()<< "(h="<<size.height<< ", w=" << size.width << ")")).str();
}

std::string Repr(const cv::Range& range) {
  return fmt::format("[{},{})", range.start, range.end);
//   return (static_cast<std::stringstream&>(std::stringstream()<< "[h="<<range.start<< ", " << range.end<< ")")).str();
}

}  // namespace sv
