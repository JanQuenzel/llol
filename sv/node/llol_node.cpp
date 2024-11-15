#include "sv/node/llol_node.h"
#include <absl/strings/match.h>

#include "sv/node/viz.h"

#include <tbb/parallel_for.h>

namespace sv {

static constexpr double kMaxRange = 32.0;

OdomNode::OdomNode(const ros::NodeHandle& pnh)
    : pnh_{pnh}, it_{pnh}, tf_listener_{tf_buffer_} {
  sub_camera_ = it_.subscribeCamera("image", 8, &OdomNode::CameraCb, this);
  sub_lidar_ = pnh_.subscribe("cloud", 8, &OdomNode::LidarCb, this);
  sub_imu_ = pnh_.subscribe(
      "imu", 100, &OdomNode::ImuCb, this, ros::TransportHints().tcpNoDelay());

  odom_frame_ = pnh_.param<std::string>("odom_frame", "odom");
  ROS_INFO_STREAM("Odom frame: " << odom_frame_);

  vis_ = pnh_.param<bool>("vis", true);
  ROS_INFO_STREAM("Visualize: " << (vis_ ? "True" : "False"));

  pc_row_major_ = pnh_.param<bool>("pc_row_major", true);
  ROS_INFO_STREAM("PointCloudRowMajor: " << (pc_row_major_ ? "True" : "False"));

  stamp_at_the_front_ = pnh_.param<bool>("stamp_at_the_front", false);
  ROS_INFO_STREAM("StampAtTheFront: " << (stamp_at_the_front_ ? "True" : "False"));

  tbb_ = pnh_.param<int>("tbb", 0);
  ROS_INFO_STREAM("Tbb grainsize: " << tbb_);

  log_ = pnh_.param<int>("log", 0);
  ROS_INFO_STREAM("Log interval: " << log_);

  tf_overwrite_ = pnh_.param<bool>("tf_overwrite", false);

  rigid_ = pnh_.param<bool>("rigid", true);
  ROS_WARN_STREAM("GICP: " << (rigid_ ? "Rigid" : "Linear"));

  path_dist_ = pnh_.param<double>("path_dist", 0.01);

  imuq_ = InitImuq({pnh_, "imuq"});
  ROS_INFO_STREAM(imuq_);

  pano_ = InitPano({pnh_, "pano"});
  ROS_INFO_STREAM(pano_);
}

void OdomNode::ImuCb(const sensor_msgs::Imu& imu_msg) {
  if (imu_frame_.empty()) {
    std::string frame_id = imu_msg.header.frame_id;
    if ( !frame_id.empty() && frame_id[0] == '/' )
      frame_id.erase(0,1);
    imu_frame_ = frame_id;
    ROS_INFO_STREAM("Imu frame: " << imu_frame_);
  }

  // Add imu data to buffer
  static std_msgs::Header prev_header;
  if (prev_header.seq > 0 && prev_header.seq + 1 != imu_msg.header.seq) {
    ROS_ERROR_STREAM("Missing imu data, prev: " << prev_header.seq << ", curr: "
                                                << imu_msg.header.seq);
  }
  prev_header = imu_msg.header;

  const auto imu = MakeImu(imu_msg);
  imuq_.Add(imu);

  if (tf_init_) return;

  if (lidar_frame_.empty()) {
    ROS_WARN_STREAM("Lidar not initialized");
    return;
  }

  if (!imuq_.full()) {
//    ROS_WARN_STREAM(fmt::format(
//        "Imu queue not full: {}/{}", imuq_.size(), imuq_.capacity()));
    ROS_WARN_STREAM("Imu queue not full: " << imuq_.size() <<" / " << imuq_.capacity());
    return;
  }

  // Get tf between imu and lidar and initialize gravity direction
  try {
    const auto tf_i_l = tf_buffer_.lookupTransform(
        imu_frame_, lidar_frame_, ros::Time(0));

    const auto& t = tf_i_l.transform.translation;
    const auto& q = tf_i_l.transform.rotation;
    const Eigen::Vector3d t_i_l{t.x, t.y, t.z};
//    const Eigen::Quaterniond q_i_l{q.w, q.x, q.y, q.z};
    const Eigen::Quaterniond q_i_l = tf_overwrite_ ?
         Eigen::Quaterniond(0, 1, -1, 0).normalized() :
         Eigen::Quaterniond(q.w, q.x, q.y, q.z);

    // Just use current acc
    ROS_INFO_STREAM("buffer size: " << imuq_.size());
    const auto imu_mean = imuq_.CalcMean(10);
    const auto& imu0 = imuq_.buf.front();

    ROS_INFO_STREAM("acc_curr: " << imu.acc.transpose()
                                 << ", norm: " << imu.acc.norm());
    ROS_INFO_STREAM("acc_mean: " << imu_mean.acc.transpose()
                                 << ", norm: " << imu_mean.acc.norm());

    // Use the one that is closer to 9.8
    const Sophus::SE3d T_i_l{q_i_l, t_i_l};
    const auto curr_acc_norm = imu.acc.norm();
    const auto mean_acc_norm = imu_mean.acc.norm();
    const auto gnorm = traj_.gravity_norm;

    if (gnorm > 0 &&
        std::abs(curr_acc_norm - gnorm) < std::abs(mean_acc_norm - gnorm)) {
      ROS_INFO_STREAM("Use curr acc as gravity");
      traj_.Init(T_i_l, imu.acc);
    } else {
      ROS_INFO_STREAM("Use mean acc as gravity");
      traj_.Init(T_i_l, imu_mean.acc);
    }

    ROS_INFO_STREAM(traj_);
    tf_init_ = true;
  } catch (tf2::TransformException& ex) {
    ROS_WARN_STREAM(ex.what());
    return;
  }
}

void OdomNode::Initialize(const sensor_msgs::CameraInfo& cinfo_msg) {
  ROS_INFO_STREAM("+++ Initializing");
  sweep_ = MakeSweep(cinfo_msg);
  ROS_INFO_STREAM(sweep_);

  grid_ = InitGrid({pnh_, "grid"}, sweep_.size());
  ROS_INFO_STREAM(grid_);

  traj_ = InitTraj({pnh_, "traj"}, grid_.cols());
  ROS_INFO_STREAM(traj_);

  gicp_ = InitGicp({pnh_, "gicp"});
  ROS_INFO_STREAM(gicp_);
}

void OdomNode::LidarCb(const sensor_msgs::PointCloud2ConstPtr& cloud_msg) {
    if ( !cloud_msg->is_dense ) return;

    constexpr float freq = 20.f;
    constexpr int range_scale = 512;
    constexpr uint32_t pixel_step = sizeof(float)*4;
    const uint32_t point_step = cloud_msg->point_step;
    const size_t num_points = cloud_msg->width * cloud_msg->height;

    static sensor_msgs::CameraInfo::Ptr cinfo_msg = nullptr; ( new sensor_msgs::CameraInfo );
    static sensor_msgs::Image::Ptr image_msg  = nullptr;

    static const uint32_t pixel_shift_by_row_os0_128[128] ={64,43,22,1,63,42,22,3,62,42,23,3,61,42,23,4,60,41,23,5,59,41,23,6,59,41,23,6,58,41,24,7,58,41,24,7,57,40,24,7,57,40,24,8,57,40,24,8,56,40,24,8,56,40,24,8,56,40,24,8,56,40,24,8,56,40,24,8,56,40,24,8,56,40,24,8,56,40,24,8,56,40,24,8,56,40,24,7,57,40,24,7,57,40,23,7,57,40,23,6,58,40,23,6,58,41,23,5,59,41,23,4,59,41,22,4,60,41,22,3,61,41,22,2,62,42,21,0};
    static const uint32_t pixel_shift_by_row_os1_64[64] ={19,12,6,0,19,13,6,0,19,13,7,1,19,13,7,1,19,13,7,1,19,13,7,1,19,13,7,1,19,13,7,1,19,13,7,1,19,13,7,1,19,13,7,1,19,13,7,1,19,13,7,1,19,13,7,1,20,13,7,1,20,14,8,1};
    if ( ! cinfo_msg  )
    {
        cinfo_msg = sensor_msgs::CameraInfo::Ptr( new sensor_msgs::CameraInfo );
        image_msg = sensor_msgs::Image::Ptr( new sensor_msgs::Image );
        cinfo_msg->width = cloud_msg->width;
        cinfo_msg->height = cloud_msg->height;
        cinfo_msg->K[0] = 1.0 / freq / cloud_msg->width; // dt
        cinfo_msg->R[0] = range_scale; // range scale
        cinfo_msg->binning_x = 0; // iscan;
        cinfo_msg->binning_y = 0; // model.cols / cloud_msg->width;
        cinfo_msg->roi.x_offset = 0; //iscan * cloud_msg->width;
        cinfo_msg->roi.y_offset = 0;
        cinfo_msg->roi.width = cloud_msg->width;
        cinfo_msg->roi.height = cloud_msg->height;
        cinfo_msg->roi.do_rectify = false;

        image_msg->data.resize( num_points * pixel_step );
        image_msg->step = cloud_msg->width * pixel_step;
        image_msg->encoding = "32FC4";
        image_msg->height = cloud_msg->height;
        image_msg->width = cloud_msg->width;
    }

    cinfo_msg->header = cloud_msg->header;
    image_msg->header = cloud_msg->header;

    std::string frame_id = cloud_msg->header.frame_id;
    if ( !frame_id.empty() && frame_id[0] == '/' )
      frame_id.erase(0,1);
    cinfo_msg->header.frame_id = frame_id;
    image_msg->header.frame_id = frame_id;


    constexpr uint32_t pixel_range_offset = sizeof(float)*3;
    constexpr uint32_t pixel_intensity_offset = sizeof(float)*3+sizeof(uint16_t);
    uint32_t offset_reflectivity = 0, offset_time = 0;
    for(size_t i=0; i<cloud_msg->fields.size(); ++i)
    {
        if ( cloud_msg->fields[i].name=="reflectivity" )
            offset_reflectivity = cloud_msg->fields[i].offset;
        if ( cloud_msg->fields[i].name=="t" )
            offset_time = cloud_msg->fields[i].offset;
    }

    //cv::Mat img = cv::Mat(cloud_msg->height, cloud_msg->width, CV_8UC1);
    //ROS_INFO_STREAM( "size: h: " << cloud_msg->height << " w: " << cloud_msg->width << " np: " << num_points);

    const uint32_t * const pixel_shift_by_row = cloud_msg->height == 128 ? &pixel_shift_by_row_os0_128[0] : &pixel_shift_by_row_os1_64[0];
    const uint32_t add_offset = cloud_msg->height == 128 ? 33 : 15;
    constexpr float max_dist = 127.f;
    if ( pc_row_major_ )
    {
        tbb::parallel_for(
            tbb::blocked_range<int>(0, num_points), [&](const auto& blk) {
              for (int i = blk.begin(); i < blk.end(); ++i)
        //for ( size_t i = 0; i < num_points; ++i )
        {
            // the cloud is row major and image should be too ( since step = width * sizeof(pt) )
            // + pc is row major aka, first row of width, than second row of width etc...
            const uint32_t row = i / cloud_msg->width; // this row should be correct
            const uint32_t col = i - row * cloud_msg->width;
            const uint32_t col_shifted = (col + cloud_msg->width - pixel_shift_by_row[row]+add_offset) % cloud_msg->width;
            const uint32_t i_new = row * cloud_msg->width + col_shifted;

            const uint32_t point_start = point_step * i_new;
            const uint32_t pixel_start = pixel_step * i;

            const Eigen::Vector3f p = Eigen::Map<const Eigen::Vector3f>(reinterpret_cast<const float*>(&cloud_msg->data[point_start]));
            const float dist = p.norm();
            const uint16_t scale_val = dist < max_dist;
            Eigen::Map<Eigen::Vector3f>((float*)&image_msg->data[pixel_start]) = scale_val * p;
            *reinterpret_cast<uint16_t*>(&image_msg->data[pixel_start + pixel_range_offset]) = scale_val * dist * range_scale;
            *reinterpret_cast<uint16_t*>(&image_msg->data[pixel_start + pixel_intensity_offset]) = scale_val *
                    *reinterpret_cast<const uint16_t*>(&cloud_msg->data[point_start + offset_reflectivity]);
        //    img.at<uint8_t>(row,col) = std::max<int>(std::min<int>(255. * p.norm() / 25., 255),0);
        }
        });
    }
    else {
        tbb::parallel_for(
            tbb::blocked_range<int>(0, num_points), [&](const auto& blk) {
              for (int i = blk.begin(); i < blk.end(); ++i)
        //for ( size_t i = 0; i < num_points; ++i )
        {
//            // the cloud is col major
//            const int row = i % cloud_msg->height;
//            const int col = i / cloud_msg->height;
//            const uint32_t i_new = col + row * cloud_msg->width;
//            //const uint32_t i_new = row + col * cloud_msg->height;
//            const uint32_t point_start = point_step * i;
//            const uint32_t pixel_start = pixel_step * i_new;

            // the cloud is col major and image should be row major ( step = width * sizeof(pt) )
            // pc is col major aka, first col of height, than second col of height etc...
            const uint32_t col = i / cloud_msg->height; // this col should be correct
            const uint32_t row = i - col * cloud_msg->height;
            const uint32_t col_shifted = (col + cloud_msg->width + pixel_shift_by_row[row]-add_offset) % cloud_msg->width;
            const uint32_t i_new = col_shifted + row * cloud_msg->width;
            //const uint32_t i_new = row + cloud_msg->height * col_shifted;
            const uint32_t point_start = point_step * i;
            const uint32_t pixel_start = pixel_step * i_new;

            const Eigen::Vector3f p = Eigen::Map<const Eigen::Vector3f>(reinterpret_cast<const float*>(&cloud_msg->data[point_start]));
            const float dist = p.norm();
            const uint16_t scale_val = dist < max_dist;
            Eigen::Map<Eigen::Vector3f>((float*)&image_msg->data[pixel_start]) = scale_val * p;
            *reinterpret_cast<uint16_t*>(&image_msg->data[pixel_start + pixel_range_offset]) = scale_val * dist * range_scale;
            *reinterpret_cast<uint16_t*>(&image_msg->data[pixel_start + pixel_intensity_offset]) = scale_val *
                    *reinterpret_cast<const uint16_t*>(&cloud_msg->data[point_start + offset_reflectivity]);
        //    img.at<uint8_t>(row,col) = std::max<int>(std::min<int>(255. * p.norm() / 25., 255),0);
        }
        });
    }
    if ( stamp_at_the_front_ )
    {
        int64_t new_stamp_offset = 0;
        for ( int point_idx = num_points-1; point_idx >= 0 && new_stamp_offset == 0; --point_idx )
        {
            const uint32_t & t_ns = *reinterpret_cast<const uint32_t*>(&cloud_msg->data[cloud_msg->point_step * point_idx + offset_time]);
            if ( t_ns <= 0 ) continue;
            new_stamp_offset = t_ns;
        }
        image_msg->header.stamp += ros::Duration().fromNSec(new_stamp_offset);
        cinfo_msg->header.stamp = image_msg->header.stamp;
    }

    //cv::imwrite("./blub.png", img);
    CameraCb(image_msg, cinfo_msg);
}

void OdomNode::CameraCb(const sensor_msgs::ImageConstPtr& image_msg,
                        const sensor_msgs::CameraInfoConstPtr& cinfo_msg) {
  static std_msgs::Header prev_header;
  if (prev_header.seq > 0 && prev_header.seq + 1 != cinfo_msg->header.seq) {
    ROS_ERROR_STREAM("Missing lidar data, prev: "
                     << prev_header.seq << ", curr: " << cinfo_msg->header.seq);
  }
  prev_header = cinfo_msg->header;

  if (lidar_frame_.empty()) {
    lidar_frame_ = image_msg->header.frame_id;
    // Allocate storage for sweep, grid and matcher
    Initialize(*cinfo_msg);
    ROS_INFO_STREAM("Lidar frame: " << lidar_frame_);
    ROS_INFO_STREAM("Lidar initialized!");
  }

  if (!imuq_.full()) {
//    ROS_WARN_STREAM(fmt::format(
//        "Imu queue not full: {}/{}", imuq_.size(), imuq_.capacity()));
    ROS_WARN_STREAM("Imu queue not full: " << imuq_.size() << " / " << imuq_.capacity());
    return;
  }

  if (!tf_init_) {
    ROS_WARN_STREAM("Transform not initialized");
    return;
  }

  if (!scan_init_) {
    if (cinfo_msg->binning_x == 0) {
      scan_init_ = true;
    } else {
      ROS_WARN_STREAM("Scan not initialized");
      return;
    }
  }

  // We can always process incoming scan no matter what
  const auto scan = MakeScan(*image_msg, *cinfo_msg);
  ROS_DEBUG("Processing scan %d: [%d,%d)",
            static_cast<int>(cinfo_msg->header.seq),
            scan.curr.start,
            scan.curr.end);
  // Add scan to sweep, compute score and filter
  Preprocess(scan);

  Register();

  PostProcess();

  Logging();

  Publish(cinfo_msg->header);
}

void OdomNode::Preprocess(const LidarScan& scan) {
  // 1. Eject scan to pano, assuming traj is optimized
  int n_added = 0;
  {  // Note that at this point the new scan is not yet added to the sweep
    auto _ = tm_.Scoped("1.Pano.Add");
    n_added = pano_.Add(sweep_, scan.curr, tbb_);
  }
  sm_.GetRef("pano.add_points").Add(n_added);
  ROS_DEBUG_STREAM("[pano.Add] num added: " << n_added);

  // 2. Add current scan to sweep
  int n_points = 0;
  {  // Add scan to sweep
    auto _ = tm_.Scoped("2.Sweep.Add");
    n_points = sweep_.Add(scan);
  }
  sm_.GetRef("sweep.add").Add(n_points);
  ROS_DEBUG_STREAM("[sweep.Add] num added: " << n_points);

  // 3. Add current scan to grid
  cv::Vec2i n_cells{};
  {  // Reduce scan to grid and Filter
    auto _ = tm_.Scoped("3.Grid.Add");
    n_cells = grid_.Add(scan, tbb_);
  }
  ROS_DEBUG_STREAM("[grid.Add] Num valid cells: "
                   << n_cells[0] << ", num good cells: " << n_cells[1]);

  sm_.GetRef("grid.valid_cells").Add(n_cells[0]);
  sm_.GetRef("grid.good_cells").Add(n_cells[1]);

  // 4. Predict new scetion in trajectory
  int n_imus{};
  {  // Integarte imu to fill nominal traj
    auto _ = tm_.Scoped("4.Imu.Integrate");
    const int pred_cols = grid_.curr.size();
    const auto t0 = grid_.TimeAt(grid_.cols() - pred_cols);
    const auto dt = grid_.dt;

    // Predict the segment of traj corresponding to current grid
    n_imus = traj_.PredictNew(imuq_, t0, dt, pred_cols);
  }
  sm_.GetRef("traj.pred_imus").Add(n_imus);
  ROS_DEBUG_STREAM("[traj.Predict] num imus: " << n_imus);

  if (vis_) {
    const auto& disps = grid_.DrawCurveVar();

    Imshow("scan",
           ApplyCmap(scan.ExtractRange(),
                     1.0 / scan.scale / kMaxRange,
                     cv::COLORMAP_PINK,
                     0));
    Imshow("curve", ApplyCmap(disps[0], 1 / 0.25, cv::COLORMAP_JET));
    Imshow("var", ApplyCmap(disps[1], 1 / 0.25, cv::COLORMAP_JET));
    Imshow("filter",
           ApplyCmap(
               grid_.DrawFilter(), 1 / grid_.max_curve, cv::COLORMAP_JET));
  }
}

void OdomNode::PostProcess() {
  const auto num_good_cells = grid_.NumCandidates();
  const auto num_matches = sm_.GetRef("grid.matches").last();
  const double match_ratio = num_matches / num_good_cells;
  ROS_DEBUG_STREAM("[pano.RenderCheck] match ratio: " << match_ratio);

  auto T_p1_p2 = traj_.TfPanoLidar();
  // Algin gravity means we will just set rotation to identity
  if (pano_.align_gravity) T_p1_p2.so3() = Sophus::SO3d{};

  // We use the inverse from now on
  const auto T_p2_p1 = T_p1_p2.inverse();

  int n_render = 0;
  if (pano_.ShouldRender(T_p2_p1, match_ratio)) {
//    ROS_WARN_STREAM(
//        "=Render= " << fmt::format(
//            "sweeps: {:2.3f}, trans: {:.3f}, match: {:.2f}% = {}/{}",
//            pano_.num_sweeps,
//            T_p1_p2.translation().norm(),
//            match_ratio * 100,
//            num_matches,
//            num_good_cells));
    ROS_WARN_STREAM("=Render= sweeps: " << pano_.num_sweeps
                    << ", trans: " << T_p1_p2.translation().norm()
                    << ", match: " << (match_ratio * 100)
                    << " = " << num_matches << " / "<< num_good_cells );

    // TODO (chao): need to think about how to run this in background without
    // interfering with odom
    auto _ = tm_.Scoped("Render");
    // Render pano at the latest lidar pose wrt pano (T_p1_p2 = T_p1_lidar)
    n_render = pano_.Render(T_p2_p1.cast<float>(), tbb_);
    // Save current pano pose
    T_odom_pano_ = traj_.T_odom_pano;
    // Once rendering is done we need to update traj accordingly
    traj_.MoveFrame(T_p2_p1);
  }
  if (n_render > 0) {
    sm_.GetRef("pano.render_points").Add(n_render);
    ROS_DEBUG_STREAM("[pano.Render] num render: " << n_render);
  }

  // 7. Update sweep transforms for undistortion
  {
    auto _ = tm_.Scoped("7.Sweep.Interp");
    sweep_.Interp(traj_, tbb_);
  }

  grid_.Interp(traj_);

  if (vis_) {
    Imshow("sweep",
           ApplyCmap(sweep_.ExtractRange(),
                     1.0 / sweep_.scale / kMaxRange,
                     cv::COLORMAP_PINK,
                     0));
    const auto& disps = pano_.DrawRangeCount();
    Imshow(
        "pano",
        ApplyCmap(
            disps[0], 1.0 / DepthPixel::kScale / kMaxRange, cv::COLORMAP_PINK));
    Imshow("count",
           ApplyCmap(disps[1], 1.0 / pano_.max_cnt, cv::COLORMAP_JET));
  }
}

void OdomNode::Logging() {
  // Record total time
  TimerManager::StatsT stats;
  absl::Duration time;
  for (const auto& kv : tm_.dict()) {
    if (absl::StartsWith(kv.first, "Total")) continue;
    if (absl::StartsWith(kv.first, "Render")) continue;
    time += kv.second.last();
  }
  stats.Add(time);
  tm_.Update("Total", stats);

  if (log_ > 0) {
    ROS_INFO_STREAM_THROTTLE(log_, tm_.ReportAll(true));
  }
}

}  // namespace sv
