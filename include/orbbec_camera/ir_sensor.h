#pragma once

#include "libobsensor/hpp/Context.hpp"
#include "libobsensor/hpp/Device.hpp"
#include "libobsensor/hpp/Sensor.hpp"
#include "ros/ros.h"
#include "image_transport/image_transport.h"
#include "image_transport/camera_publisher.h"
#include "orbbec_camera/GetCameraInfo.h"

class IrSensor
{
private:
    ros::NodeHandle& mNodeHandle;
    ros::NodeHandle& mPrivateNodeHandle;
    image_transport::Publisher mIrPub;
    ros::ServiceServer mIrInfoService;

    std::shared_ptr<ob::Device> mDevice;
    std::shared_ptr<ob::Sensor> mIrSensor;
    std::shared_ptr<ob::StreamProfile> mIrProfile;

    bool getCameraInfoCallback(orbbec_camera::GetCameraInfoRequest& req, orbbec_camera::GetCameraInfoResponse& res);

public:
    IrSensor(ros::NodeHandle& nh, ros::NodeHandle& pnh, std::shared_ptr<ob::Device> device, std::shared_ptr<ob::Sensor> sensor);
    ~IrSensor();

    void startIrStream();
    void stopIrStream();
    void reconfigIrStream(int width, int height, int fps);
};
