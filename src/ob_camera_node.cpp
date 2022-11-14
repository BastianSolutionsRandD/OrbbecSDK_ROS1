#include "orbbec_camera/ob_camera_node.h"
namespace orbbec_camera {
OBCameraNode::OBCameraNode(ros::NodeHandle& nh, ros::NodeHandle& nh_private,
                           std::shared_ptr<ob::Device> device)
    : nh_(nh), nh_private_(nh_private), device_(std::move(device)) {
  init();
}

void OBCameraNode::init() {
  std::lock_guard<decltype(device_lock_)> lock(device_lock_);
  CHECK_NOTNULL(device_);
  is_running_ = true;
  setupConfig();
  getParameters();
  setupDevices();
  setupProfiles();
  setupCameraInfo();
  setupTopics();
  setupCameraCtrlServices();
  setupFrameCallback();
}

OBCameraNode::~OBCameraNode() {
  std::lock_guard<decltype(device_lock_)> lock(device_lock_);
  is_running_ = false;
  if (tf_thread_ && tf_thread_->joinable()) {
    tf_thread_->join();
  }
  stopStreams();
}

void OBCameraNode::getParameters() {
  camera_name_ = nh_private_.param<std::string>("camera_name", "camera");
  base_frame_id_ = camera_name_ + "_link";
  for (const auto& stream_index : IMAGE_STREAMS) {
    frame_id_[stream_index] = camera_name_ + "_" + stream_name_[stream_index] + "_frame";
    optical_frame_id_[stream_index] =
        camera_name_ + "_" + stream_name_[stream_index] + "_optical_frame";
  }
  for (const auto& stream_index : IMAGE_STREAMS) {
    std::string param_name = stream_name_[stream_index] + "_width";
    width_[stream_index] = nh_private_.param<int>(param_name, IMAGE_WIDTH);
    param_name = stream_name_[stream_index] + "_height";
    height_[stream_index] = nh_private_.param<int>(param_name, IMAGE_HEIGHT);
    param_name = stream_name_[stream_index] + "_fps";
    fps_[stream_index] = nh_private_.param<int>(param_name, IMAGE_FPS);
    param_name = "enable_" + stream_name_[stream_index];
    enable_[stream_index] = nh_private_.param<bool>(param_name, false);
    param_name = stream_name_[stream_index] + "_format";
    format_str_[stream_index] =
        nh_private_.param<std::string>(param_name, format_str_[stream_index]);
    format_[stream_index] = OBFormatFromString(format_str_[stream_index]);
  }
  for (const auto& stream_index : IMAGE_STREAMS) {
    depth_aligned_frame_id_[stream_index] = optical_frame_id_[COLOR];
  }
  publish_tf_ = nh_private_.param<bool>("publish_tf", true);
  depth_align_ = nh_private_.param<bool>("depth_align", false);
  ir_info_uri_ = nh_private_.param<std::string>("ir_info_uri", "");
  color_info_uri_ = nh_private_.param<std::string>("color_info_uri", "");
  enable_d2c_viewer_ = nh_private_.param<bool>("enable_d2c_viewer", false);
  enable_pipeline_ = nh_private_.param<bool>("enable_pipeline", false);
  enable_point_cloud_ = nh_private_.param<bool>("enable_point_cloud", true);
  enable_point_cloud_xyzrgb_ = nh_private_.param<bool>("enable_point_cloud_xyzrgb", true);
}

void OBCameraNode::startStreams() {
  std::lock_guard<decltype(device_lock_)> lock(device_lock_);
  if (enable_pipeline_) {
    CHECK_NOTNULL(pipeline_.get());
    try {
      setupPipelineConfig();
      pipeline_->start(pipeline_config_, [this](std::shared_ptr<ob::FrameSet> frame_set) {
        CHECK_NOTNULL(frame_set);
        this->onNewFrameSetCallback(std::move(frame_set));
      });
    } catch (const ob::Error& e) {
      ROS_ERROR_STREAM("failed to start pipeline: " << e.getMessage()
                                                    << " try to disable ir stream try again");
      enable_[INFRA0] = false;
      setupPipelineConfig();
      pipeline_->start(pipeline_config_, [this](std::shared_ptr<ob::FrameSet> frame_set) {
        CHECK_NOTNULL(frame_set);
        this->onNewFrameSetCallback(std::move(frame_set));
      });
    }
    pipeline_started_ = true;
  } else {
    for (const auto& stream_index : IMAGE_STREAMS) {
      if (enable_[stream_index] && !stream_started_[stream_index]) {
        startStream(stream_index);
      }
    }
  }
}

void OBCameraNode::stopStreams() {
  std::lock_guard<decltype(device_lock_)> lock(device_lock_);
  if (enable_pipeline_) {
    CHECK_NOTNULL(pipeline_);
    pipeline_->stop();
    pipeline_started_ = false;
  } else {
    for (const auto& stream_index : IMAGE_STREAMS) {
      if (stream_started_[stream_index]) {
        stopStream(stream_index);
      }
    }
  }
}

void OBCameraNode::startStream(const stream_index_pair& stream_index) {
  std::lock_guard<decltype(device_lock_)> lock(device_lock_);
  if (enable_pipeline_) {
    ROS_WARN_STREAM("Cannot start stream when pipeline is enabled");
    return;
  }
  if (!enable_[stream_index]) {
    ROS_WARN_STREAM("Stream " << stream_name_[stream_index] << " is not enabled, cannot start it.");
    return;
  }
  if (stream_started_[stream_index]) {
    ROS_WARN_STREAM("Stream " << stream_name_[stream_index] << " is already started.");
    return;
  }
  ROS_INFO_STREAM("Starting stream " << stream_name_[stream_index] << "...");
  bool has_subscriber = image_publishers_[stream_index].getNumSubscribers() > 0;
  if (!has_subscriber) {
    ROS_INFO_STREAM("No subscriber for stream " << stream_name_[stream_index] << ", skip it.");
    return;
  }
  CHECK_GE(stream_profile_.count(stream_index), 0u);
  CHECK_GE(frame_callback_.count(stream_index), 0u);
  CHECK_GE(sensors_.count(stream_index), 0u);
  auto callback = frame_callback_[stream_index];
  auto profile = stream_profile_[stream_index];
  try {
    sensors_[stream_index]->startStream(profile, callback);
    stream_started_[stream_index] = true;
    ROS_INFO_STREAM("Stream " << stream_name_[stream_index] << " started.");
  } catch (...) {
    ROS_ERROR_STREAM("Failed to start stream " << stream_name_[stream_index] << ".");
  }
}

void OBCameraNode::stopStream(const stream_index_pair& stream_index) {
  std::lock_guard<decltype(device_lock_)> lock(device_lock_);
  if (enable_pipeline_) {
    ROS_WARN_STREAM("Cannot stop stream when pipeline is enabled");
    return;
  }
  if (!stream_started_[stream_index]) {
    ROS_WARN_STREAM("Stream " << stream_name_[stream_index] << " is not started.");
    return;
  }
  ROS_INFO_STREAM("Stopping stream " << stream_name_[stream_index] << "...");
  sensors_[stream_index]->stopStream();
  stream_started_[stream_index] = false;
  ROS_INFO_STREAM("Stream " << stream_name_[stream_index] << " stopped.");
}

void OBCameraNode::publishPointCloud(std::shared_ptr<ob::FrameSet> frame_set) {
  try {
    if (depth_align_) {
      if (frame_set->depthFrame() != nullptr && frame_set->colorFrame() != nullptr) {
        publishColorPointCloud(frame_set);
      }
    }
    if (frame_set->depthFrame() != nullptr) {
      publishDepthPointCloud(frame_set);
    }
  } catch (const ob::Error& e) {
    ROS_ERROR_STREAM(e.getMessage());
  } catch (const std::exception& e) {
    ROS_ERROR_STREAM(e.what());
  } catch (...) {
    ROS_ERROR_STREAM("publishPointCloud with unknown error");
  }
}

void OBCameraNode::publishDepthPointCloud(std::shared_ptr<ob::FrameSet> frame_set) {
  if (depth_cloud_pub_.getNumSubscribers() == 0 || !enable_point_cloud_) {
    return;
  }
  auto camera_param = pipeline_->getCameraParam();
  cloud_filter_.setCameraParam(camera_param);
  cloud_filter_.setCreatePointFormat(OB_FORMAT_POINT);
  auto depth_frame = frame_set->depthFrame();
  auto frame = cloud_filter_.process(frame_set);
  size_t point_size = frame->dataSize() / sizeof(OBPoint);
  auto* points = (OBPoint*)frame->data();
  CHECK_NOTNULL(points);
  sensor_msgs::PointCloud2Modifier modifier(cloud_msg_);
  modifier.setPointCloud2FieldsByString(1, "xyz");
  modifier.resize(point_size);
  cloud_msg_.width = depth_frame->width();
  cloud_msg_.height = depth_frame->height();
  cloud_msg_.row_step = cloud_msg_.width * cloud_msg_.point_step;
  cloud_msg_.data.resize(cloud_msg_.height * cloud_msg_.row_step);
  sensor_msgs::PointCloud2Iterator<float> iter_x(cloud_msg_, "x");
  sensor_msgs::PointCloud2Iterator<float> iter_y(cloud_msg_, "y");
  sensor_msgs::PointCloud2Iterator<float> iter_z(cloud_msg_, "z");
  size_t valid_count = 0;
  for (size_t point_idx = 0; point_idx < point_size; point_idx++, points++) {
    bool valid_pixel(points->z > 0);
    if (valid_pixel) {
      *iter_x = static_cast<float>(points->x / 1000.0);
      *iter_y = -static_cast<float>(points->y / 1000.0);
      *iter_z = static_cast<float>(points->z / 1000.0);
      ++iter_x;
      ++iter_y;
      ++iter_z;
      ++valid_count;
    }
  }
  auto timestamp = frameTimeStampToROSTime(depth_frame->systemTimeStamp());
  cloud_msg_.header.stamp = timestamp;
  cloud_msg_.header.frame_id = optical_frame_id_[DEPTH];
  cloud_msg_.is_dense = true;
  cloud_msg_.width = valid_count;
  cloud_msg_.height = 1;
  modifier.resize(valid_count);
  depth_cloud_pub_.publish(cloud_msg_);
  if (save_point_cloud_xyz_) {
    save_point_cloud_xyz_ = false;
    auto now = std::time(nullptr);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now), "%Y%m%d_%H%M%S");
    auto current_path = boost::filesystem::current_path().string();
    std::string filename = current_path + "/point_cloud/points_xyz_" + ss.str() + ".ply";
    if (!boost::filesystem::exists(current_path + "/point_cloud")) {
      boost::filesystem::create_directory(current_path + "/point_cloud");
    }
    ROS_INFO_STREAM("Saving point cloud to " << filename);
    savePointsToPly(frame, filename);
  }
}

void OBCameraNode::publishColorPointCloud(std::shared_ptr<ob::FrameSet> frame_set) {
  if (depth_registered_cloud_pub_.getNumSubscribers() == 0 || !enable_point_cloud_xyzrgb_) {
    return;
  }
  auto depth_frame = frame_set->depthFrame();
  auto color_frame = frame_set->colorFrame();
  auto camera_param = pipeline_->getCameraParam();
  cloud_filter_.setCameraParam(camera_param);
  cloud_filter_.setCreatePointFormat(OB_FORMAT_RGB_POINT);
  auto frame = cloud_filter_.process(frame_set);
  size_t point_size = frame->dataSize() / sizeof(OBColorPoint);
  auto* points = (OBColorPoint*)frame->data();
  CHECK_NOTNULL(points);
  sensor_msgs::PointCloud2Modifier modifier(cloud_msg_);
  modifier.setPointCloud2FieldsByString(1, "xyz");
  modifier.resize(point_size);
  cloud_msg_.width = color_frame->width();
  cloud_msg_.height = color_frame->height();
  std::string format_str = "rgb";
  cloud_msg_.point_step = addPointField(cloud_msg_, format_str.c_str(), 1,
                                        sensor_msgs::PointField::FLOAT32, cloud_msg_.point_step);
  cloud_msg_.row_step = cloud_msg_.width * cloud_msg_.point_step;
  cloud_msg_.data.resize(cloud_msg_.height * cloud_msg_.row_step);
  sensor_msgs::PointCloud2Iterator<float> iter_x(cloud_msg_, "x");
  sensor_msgs::PointCloud2Iterator<float> iter_y(cloud_msg_, "y");
  sensor_msgs::PointCloud2Iterator<float> iter_z(cloud_msg_, "z");
  sensor_msgs::PointCloud2Iterator<uint8_t> iter_r(cloud_msg_, "r");
  sensor_msgs::PointCloud2Iterator<uint8_t> iter_g(cloud_msg_, "g");
  sensor_msgs::PointCloud2Iterator<uint8_t> iter_b(cloud_msg_, "b");
  size_t valid_count = 0;

  for (size_t point_idx = 0; point_idx < point_size; point_idx += 1) {
    bool valid_pixel((points + point_idx)->z > 0);
    if (valid_pixel) {
      *iter_x = static_cast<float>((points + point_idx)->x / 1000.0);
      *iter_y = -static_cast<float>((points + point_idx)->y / 1000.0);
      *iter_z = static_cast<float>((points + point_idx)->z / 1000.0);
      *iter_r = static_cast<uint8_t>((points + point_idx)->r);
      *iter_g = static_cast<uint8_t>((points + point_idx)->g);
      *iter_b = static_cast<uint8_t>((points + point_idx)->b);

      ++iter_x;
      ++iter_y;
      ++iter_z;
      ++iter_r;
      ++iter_g;
      ++iter_b;
      ++valid_count;
    }
  }
  auto timestamp = frameTimeStampToROSTime(depth_frame->systemTimeStamp());
  cloud_msg_.header.stamp = timestamp;
  cloud_msg_.header.frame_id = optical_frame_id_[COLOR];
  cloud_msg_.is_dense = true;
  cloud_msg_.width = valid_count;
  cloud_msg_.height = 1;
  modifier.resize(valid_count);
  depth_registered_cloud_pub_.publish(cloud_msg_);
  if (save_point_cloud_xyzrgb_) {
    save_point_cloud_xyz_ = false;
    auto now = std::time(nullptr);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now), "%Y%m%d_%H%M%S");
    auto current_path = boost::filesystem::current_path().string();
    std::string filename = current_path + "/point_cloud/points_xyzrgb_" + ss.str() + ".ply";
    if (!boost::filesystem::exists(current_path + "/point_cloud")) {
      boost::filesystem::create_directory(current_path + "/point_cloud");
    }
    ROS_INFO_STREAM("Saving point cloud to " << filename);
    saveRGBPointsToPly(frame, filename);
  }
}

void OBCameraNode::onNewFrameSetCallback(std::shared_ptr<ob::FrameSet> frame_set) {
  if (frame_set == nullptr) {
    return;
  }
  onNewFrameCallback(frame_set->colorFrame(), COLOR);
  onNewFrameCallback(frame_set->depthFrame(), DEPTH);
  onNewFrameCallback(frame_set->irFrame(), INFRA0);
  publishPointCloud(frame_set);
}

void OBCameraNode::onNewFrameCallback(std::shared_ptr<ob::Frame> frame,
                                      const stream_index_pair& stream_index) {
  if (frame == nullptr) {
    return;
  }
  std::shared_ptr<ob::VideoFrame> video_frame;
  if (frame->type() == OB_FRAME_COLOR && frame->format() != OB_FORMAT_RGB888) {
    if (!setupFormatConvertType(frame->format())) {
      ROS_ERROR_STREAM("Unsupported color format: " << frame->format());
      return;
    }
    video_frame = format_convert_filter_.process(frame)->as<ob::ColorFrame>();
  } else if (frame->type() == OB_FRAME_COLOR) {
    video_frame = frame->as<ob::ColorFrame>();
  } else if (frame->type() == OB_FRAME_DEPTH) {
    video_frame = frame->as<ob::DepthFrame>();
  } else if (frame->type() == OB_FRAME_IR) {
    video_frame = frame->as<ob::IRFrame>();
  } else {
    ROS_ERROR_STREAM("Unsupported frame type: " << frame->type());
    return;
  }
  if (!video_frame) {
    ROS_ERROR_STREAM("Failed to convert frame to video frame");
    return;
  }
  int width = static_cast<int>(video_frame->width());
  int height = static_cast<int>(video_frame->height());
  auto& image = images_[stream_index];
  if (image.empty() || image.cols != width || image.rows != height) {
    image.create(height, width, image_format_[stream_index]);
  }
  image.data = (uchar*)video_frame->data();
  auto timestamp = frameTimeStampToROSTime(video_frame->systemTimeStamp());
  if (camera_infos_.count(stream_index)) {
    auto camera_info = camera_infos_[stream_index];
    auto camera_info_publisher = camera_info_publishers_[stream_index];
    camera_info.header.stamp = timestamp;
    camera_info_publisher.publish(camera_info);
  }
  CHECK(image_publishers_.count(stream_index));
  auto image_publisher = image_publishers_[stream_index];
  auto image_msg =
      cv_bridge::CvImage(std_msgs::Header(), encoding_[stream_index], image).toImageMsg();
  image_msg->header.stamp = timestamp;
  image_msg->is_bigendian = false;
  image_msg->step = width * unit_step_size_[stream_index];
  image_msg->header.frame_id =
      depth_align_ ? depth_aligned_frame_id_[stream_index] : optical_frame_id_[stream_index];
  image_publisher.publish(image_msg);
  if (save_images_[stream_index]) {
    auto now = std::time(nullptr);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now), "%Y%m%d_%H%M%S");
    auto current_path = boost::filesystem::current_path().string();
    auto fps = fps_[stream_index];
    std::string filename = current_path + "/image/" + stream_name_[stream_index] + "_" +
                           std::to_string(image_msg->width) + "x" +
                           std::to_string(image_msg->height) + "_" + std::to_string(fps) + "hz_" +
                           ss.str() + ".jpg";
    if (!boost::filesystem::exists(current_path + "/image")) {
      boost::filesystem::create_directory(current_path + "/image");
    }
    ROS_INFO_STREAM("Saving image to " << filename);
    cv::imwrite(filename, image);
    save_images_[stream_index] = false;
  }
}

void OBCameraNode::imageSubscribedCallback(const stream_index_pair& stream_index) {
  ROS_INFO_STREAM("Image stream " << stream_name_[stream_index] << " subscribed");
  std::lock_guard<decltype(device_lock_)> lock(device_lock_);
  if (enable_pipeline_) {
    if (pipeline_started_) {
      ROS_WARN_STREAM("pipe line already started");
      return;
    }
    startStreams();
  } else {
    if (stream_started_[stream_index]) {
      ROS_INFO_STREAM("Stream " << stream_name_[stream_index] << " is already started.");
      return;
    }
    startStream(stream_index);
  }
}

void OBCameraNode::imageUnsubscribedCallback(const stream_index_pair& stream_index) {
  ROS_INFO_STREAM("Image stream " << stream_name_[stream_index] << " unsubscribed");
  std::lock_guard<decltype(device_lock_)> lock(device_lock_);
  if (enable_pipeline_) {
    if (!pipeline_started_) {
      ROS_WARN_STREAM("pipe line not start");
      return;
    }
    bool all_stream_no_subscriber = true;
    for (auto& item : image_publishers_) {
      if (item.second.getNumSubscribers() > 0) {
        all_stream_no_subscriber = false;
      }
    }
    if (all_stream_no_subscriber) {
      stopStreams();
    }
  } else {
    if (!stream_started_[stream_index]) {
      ROS_INFO_STREAM("Stream " << stream_name_[stream_index] << " is not started.");
      return;
    }
    auto subscriber_count = image_publishers_[stream_index].getNumSubscribers();
    if (subscriber_count == 0) {
      stopStream(stream_index);
    }
  }
}

void OBCameraNode::pointCloudXYZSubscribedCallback() {
  ROS_INFO_STREAM("point cloud subscribed");
  imageSubscribedCallback(DEPTH);
}

void OBCameraNode::pointCloudXYZUnsubscribedCallback() {
  ROS_INFO_STREAM("point cloud unsubscribed");
  imageUnsubscribedCallback(DEPTH);
}

void OBCameraNode::pointCloudXYZRGBSubscribedCallback() {
  ROS_INFO_STREAM("rgb point cloud subscribed");
  imageSubscribedCallback(DEPTH);
  imageSubscribedCallback(COLOR);
}

void OBCameraNode::pointCloudXYZRGBUnsubscribedCallback() {
  ROS_INFO_STREAM("point cloud unsubscribed");
  imageUnsubscribedCallback(DEPTH);
  imageUnsubscribedCallback(COLOR);
}

boost::optional<OBCameraParam> OBCameraNode::getCameraParam() {
  auto camera_params = device_->getCalibrationCameraParamList();
  for (size_t i = 0; i < camera_params->count(); i++) {
    auto param = camera_params->getCameraParam(i);
    int depth_w = param.depthIntrinsic.width;
    int depth_h = param.depthIntrinsic.height;
    int color_w = param.rgbIntrinsic.width;
    int color_h = param.rgbIntrinsic.height;
    if ((depth_w * height_[DEPTH] == depth_h * width_[DEPTH]) &&
        (color_w * height_[COLOR] == color_h * width_[COLOR])) {
      return param;
    }
  }
  return {};
}

int OBCameraNode::getCameraParamIndex() {
  auto camera_params = device_->getCalibrationCameraParamList();
  for (size_t i = 0; i < camera_params->count(); i++) {
    auto param = camera_params->getCameraParam(i);
    int depth_w = param.depthIntrinsic.width;
    int depth_h = param.depthIntrinsic.height;
    int color_w = param.rgbIntrinsic.width;
    int color_h = param.rgbIntrinsic.height;
    if ((depth_w * height_[DEPTH] == depth_h * width_[DEPTH]) &&
        (color_w * height_[COLOR] == color_h * width_[COLOR])) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void OBCameraNode::publishStaticTF(const ros::Time& t, const std::vector<float>& trans,
                                   const tf2::Quaternion& q, const std::string& from,
                                   const std::string& to) {
  CHECK_EQ(trans.size(), 3u);
  geometry_msgs::TransformStamped msg;
  msg.header.stamp = t;
  msg.header.frame_id = from;
  msg.child_frame_id = to;
  msg.transform.translation.x = trans.at(2) / 1000.0;
  msg.transform.translation.y = -trans.at(0) / 1000.0;
  msg.transform.translation.z = -trans.at(1) / 1000.0;
  msg.transform.rotation.x = q.getX();
  msg.transform.rotation.y = q.getY();
  msg.transform.rotation.z = q.getZ();
  msg.transform.rotation.w = q.getW();
  static_tf_msgs_.push_back(msg);
}

void OBCameraNode::calcAndPublishStaticTransform() {
  tf2::Quaternion quaternion_optical, zero_rot, Q;
  std::vector<float> trans(3, 0);
  zero_rot.setRPY(0.0, 0.0, 0.0);
  quaternion_optical.setRPY(-M_PI / 2, 0.0, -M_PI / 2);
  std::vector<float> zero_trans = {0, 0, 0};
  auto camera_param = getCameraParam();
  if (camera_param) {
    auto ex = camera_param->transform;
    Q = rotationMatrixToQuaternion(ex.rot);
    Q = quaternion_optical * Q * quaternion_optical.inverse();
    extrinsics_publisher_.publish(obExtrinsicsToMsg(ex, "depth_to_color_extrinsics"));
  } else {
    Q.setRPY(0, 0, 0);
  }
  auto tf_timestamp = ros::Time::now();

  publishStaticTF(tf_timestamp, trans, Q, frame_id_[DEPTH], frame_id_[COLOR]);
  publishStaticTF(tf_timestamp, zero_trans, quaternion_optical, frame_id_[COLOR],
                  optical_frame_id_[COLOR]);
  publishStaticTF(tf_timestamp, zero_trans, quaternion_optical, frame_id_[DEPTH],
                  optical_frame_id_[DEPTH]);
  publishStaticTF(tf_timestamp, zero_trans, zero_rot, camera_link_frame_id_, frame_id_[DEPTH]);
}

void OBCameraNode::publishDynamicTransforms() {
  ROS_WARN("Publishing dynamic camera transforms (/tf) at %g Hz", tf_publish_rate_);
  static std::mutex mu;
  std::unique_lock<std::mutex> lock(mu);
  while (ros::ok() && is_running_) {
    tf_cv_.wait_for(lock, std::chrono::milliseconds((int)(1000.0 / tf_publish_rate_)),
                    [this] { return (!(is_running_)); });
    {
      ros::Time t = ros::Time::now();
      for (auto& msg : static_tf_msgs_) {
        msg.header.stamp = t;
      }
      dynamic_tf_broadcaster_->sendTransform(static_tf_msgs_);
    }
  }
}

void OBCameraNode::publishStaticTransforms() {
  static_tf_broadcaster_ = std::make_shared<tf2_ros::StaticTransformBroadcaster>();
  dynamic_tf_broadcaster_ = std::make_shared<tf2_ros::TransformBroadcaster>();
  calcAndPublishStaticTransform();
  if (tf_publish_rate_ > 0) {
    tf_thread_ = std::make_shared<std::thread>([this]() { publishDynamicTransforms(); });
  } else {
    static_tf_broadcaster_->sendTransform(static_tf_msgs_);
  }
}

}  // namespace orbbec_camera
