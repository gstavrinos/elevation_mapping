/*
 * ElevationMap.cpp
 *
 *  Created on: Nov 12, 2013
 *      Author: Péter Fankhauser
 *	 Institute: ETH Zurich, Autonomous Systems Lab
 */
#include "ElevationMap.hpp"

// StarlETH Navigation
#include <starleth_elevation_msg/ElevationMap.h>
#include <EigenConversions.hpp>
#include <TransformationMath.hpp>

// PCL
#include <pcl/common/transforms.h>
#include <pcl/filters/passthrough.h>

// ROS
#include <tf_conversions/tf_eigen.h>

using namespace std;
using namespace Eigen;
using namespace pcl;
using namespace ros;
using namespace tf;

namespace starleth_elevation_map {

ElevationMap::ElevationMap(ros::NodeHandle& nodeHandle)
    : nodeHandle_(nodeHandle)
{
  ROS_INFO("StarlETH elevation map node started.");
  readParameters();
  pointCloudSubscriber_ = nodeHandle_.subscribe(pointCloudTopic_, 1, &ElevationMap::pointCloudCallback, this);
  elevationMapPublisher_ = nodeHandle_.advertise<starleth_elevation_msg::ElevationMap>("elevation_map", 1);
  mapUpdateTimer_ = nodeHandle_.createTimer(maxNoUpdateDuration_ * 0.5, &ElevationMap::mapUpdateTimerCallback, this); // Need to check at double rate.
  initialize();
}

ElevationMap::~ElevationMap()
{

}

bool ElevationMap::readParameters()
{
  nodeHandle_.param("point_cloud_topic", pointCloudTopic_, string("/depth_registered/points_throttled"));
  nodeHandle_.param("map_frame_id", parentFrameId_, string("/map"));
  nodeHandle_.param("elevation_map_id", elevationMapFrameId_, string("/elevation_map"));
  nodeHandle_.param("sensor_cutoff_depth", sensorCutoffDepth_, 3.0);
  nodeHandle_.param("length_in_x", length_(0), 3.0);
  nodeHandle_.param("length_in_y", length_(1), 3.0);
  nodeHandle_.param("resolution", resolution_, 0.01); ROS_ASSERT(resolution_ > 0.0);
  nodeHandle_.param("min_variance", minVariance_, 0.001); ROS_ASSERT(minVariance_ > 0.0);
  nodeHandle_.param("max_variance", maxVariance_, 0.5); ROS_ASSERT(maxVariance_ > 0.0);

  double minUpdateRate;
  nodeHandle_.param("min_update_rate", minUpdateRate, 2.0); ROS_ASSERT(minUpdateRate > 0.0);
  maxNoUpdateDuration_.fromSec(1.0 / minUpdateRate);

  elevationMapToParentTransform_.setIdentity();
  elevationMapToParentTransform_.translation().x() = 0.8;

  return true;
}

bool ElevationMap::initialize()
{
  resize(length_);
  reset();
  broadcastElevationMapTransform(Time::now());
  Duration(1.0).sleep(); // Need this to get the TF caches fill up.
  ROS_INFO("StarlETH elevation map node initialized.");
  return true;
}

void ElevationMap::pointCloudCallback(
    const sensor_msgs::PointCloud2& rawPointCloud)
{
  // Convert the sensor_msgs/PointCloud2 data to pcl/PointCloud
  PointCloud<PointXYZRGB>::Ptr pointCloud(new PointCloud<PointXYZRGB>);
  fromROSMsg(rawPointCloud, *pointCloud);
  ROS_DEBUG("ElevationMap received a point cloud (%i points) for elevation mapping.", pointCloud->width * pointCloud->height);

  // Callback procedure
  Time& time = pointCloud->header.stamp;
  if (!broadcastElevationMapTransform(time)) ROS_ERROR("ElevationMap: Broadcasting elevation map transform to parent failed.");
  if (!updateProcessNoise(time)) { ROS_ERROR("ElevationMap: Updating process noise failed."); return; }
  setTimeOfLastUpdate(time);
  cleanPointCloud(pointCloud);
  if (!transformPointCloud(pointCloud, elevationMapFrameId_)) ROS_ERROR("ElevationMap: Point cloud transform failed for time stamp %f.", time.toSec());
  else if (!addToElevationMap(pointCloud)) ROS_ERROR("ElevationMap: Adding point cloud to elevation map failed.");
  if (!publishElevationMap()) ROS_ERROR("ElevationMap: Broadcasting elevation map failed.");
}

void ElevationMap::mapUpdateTimerCallback(const ros::TimerEvent& timerEvent)
{
  if (Time::now() - timeOfLastUpdate_ < maxNoUpdateDuration_) return;

  ROS_DEBUG("Elevation map is updated without data from the sensor.");

  // Callback procedure
  Time time = Time::now();
  if (!broadcastElevationMapTransform(time)) ROS_ERROR("ElevationMap: Broadcasting elevation map transform to parent failed.");

}

bool ElevationMap::broadcastElevationMapTransform(const ros::Time& time)
{
  tf::Transform tfTransform;
  poseEigenToTF(elevationMapToParentTransform_, tfTransform);
  transformBroadcaster_.sendTransform(tf::StampedTransform(tfTransform, time, parentFrameId_, elevationMapFrameId_));
  ROS_DEBUG("Published transform for elevation map in parent frame at time %f.", time.toSec());
  return true;
}

bool ElevationMap::updateProcessNoise(const ros::Time& time)
{
  // TODO Add variance depending on movement from previous update time
  varianceData_ = (varianceData_.array() + 0.005).matrix();
  varianceDataX_ = (varianceDataX_.array() + 0.005).matrix();
  varianceDataY_ = (varianceDataY_.array() + 0.005).matrix();
  varianceData_ = varianceData_.unaryExpr(VarianceClampOperator<double>(minVariance_, maxVariance_));
  return true;
}

bool ElevationMap::cleanPointCloud(const PointCloud<PointXYZRGB>::Ptr pointCloud)
{
  PassThrough<PointXYZRGB> passThroughFilter;
  PointCloud<PointXYZRGB> tempPointCloud;

  passThroughFilter.setInputCloud(pointCloud);
  passThroughFilter.setFilterFieldName("z");
  passThroughFilter.setFilterLimits(0.0, sensorCutoffDepth_);
  //! This makes the point cloud also dense (no NaN points).
  passThroughFilter.filter(tempPointCloud);
  tempPointCloud.is_dense = true;
  pointCloud->swap(tempPointCloud);

  int pointCloudSize = pointCloud->width * pointCloud->height;
  ROS_DEBUG("ElevationMap: cleanPointCloud() reduced point cloud to %i points.", pointCloudSize);
  return true;
}

bool ElevationMap::transformPointCloud(
    const PointCloud<PointXYZRGB>::Ptr pointCloud,
    const std::string& targetFrame)
{
  StampedTransform transformTf;
  string sourceFrame = pointCloud->header.frame_id;
  Time timeStamp =  pointCloud->header.stamp;

  PointCloud<PointXYZRGB>::Ptr pointCloudTransformed(new PointCloud<PointXYZRGB>);

  try
  {
    transformListener_.waitForTransform(targetFrame, sourceFrame, timeStamp, ros::Duration(maxNoUpdateDuration_));
    transformListener_.lookupTransform(targetFrame, sourceFrame, timeStamp, transformTf);
    Affine3d transform;
    poseTFToEigen(transformTf, transform);
    pcl::transformPointCloud(*pointCloud, *pointCloudTransformed, transform.cast<float>());
    pointCloud->swap(*pointCloudTransformed);
    pointCloud->header.frame_id = targetFrame;
//    pointCloud->header.stamp = timeStamp;
    ROS_DEBUG("ElevationMap: Point cloud transformed for time stamp %f.", timeStamp.toSec());
    return true;
  }
  catch (TransformException &ex)
  {
    ROS_ERROR("%s", ex.what());
    return false;
  }
}

bool ElevationMap::addToElevationMap(
    const pcl::PointCloud<pcl::PointXYZRGB>::Ptr pointCloud)
{
  int pointCloudSize = pointCloud->width * pointCloud->height;

  for (unsigned int i = 0; i < pointCloudSize; ++i)
  {
    auto& point = pointCloud->points[i];

    Array2i index;
    Vector2d position(point.x, point.y);
    if(!starleth_elevation_msg::getIndexFromPosition(index, position, length_, resolution_)) continue;

    auto& elevation = elevationData_(index(0), index(1));
    auto& variance = varianceData_(index(0), index(1));
    auto& varianceX = varianceDataX_(index(0), index(1));
    auto& varianceY = varianceDataY_(index(0), index(1));
    auto& color = colorData_(index(0), index(1));

    double measurementVariance = 0.3;

    if (std::isnan(elevation))
    {
      elevation = point.z;
      variance = measurementVariance;
      varianceX = measurementVariance;
      varianceY = measurementVariance;
    }
    else
    {
      elevation = (variance * point.z + measurementVariance * elevation) / (variance + measurementVariance);
      variance = (measurementVariance * variance) / (measurementVariance + variance);
      varianceX = (measurementVariance * variance) / (measurementVariance + variance);
      varianceY = (measurementVariance * variance) / (measurementVariance + variance);
    }

    color = ((int)point.r) << 16 | ((int)point.g) << 8 | ((int)point.b);
  }

//  bool local_map_is_subscribed = (pub_local_map.getNumSubscribers () > 0);
//  bool global_map_is_subscribed = (pub_global_map.getNumSubscribers () > 0);
//
//  if(local_map_is_subscribed)
//      local_elevation_map.data = std::vector<int16_t>(elevation_map_meta.width * elevation_map_meta.height,(int16_t)-elevation_map_meta.zero_elevation);
//
//  unsigned int size = (unsigned int)pointcloud2_map_pcl->points.size();
//
//  // iterate trough all points
//  for (unsigned int k = 0; k < size; ++k)
//  {
//      const pcl::PointXYZ& pt_cloud = pointcloud2_map_pcl->points[k];
//
//      double measurement_distance = pointcloud2_sensor_pcl->points[k].z;
//
//      // check for invalid measurements
//      if (isnan(pt_cloud.x) || isnan(pt_cloud.y) || isnan(pt_cloud.z))
//          continue;
//
//      // check max distance (manhatten norm)
//      if(max_observable_distance < measurement_distance)
//          continue;
//
//      // check min/max height
//      if(elevation_map_meta.min_elevation+local_map_transform.getOrigin().z() > pt_cloud.z || elevation_map_meta.max_elevation+local_map_transform.getOrigin().z() < pt_cloud.z)
//          continue;
//
//      // allign grid points
//      Eigen::Vector2f index_world(pt_cloud.x, pt_cloud.y);
//      Eigen::Vector2f index_map (world_map_transform.getC2Coords(index_world));
//
//      unsigned int cell_index = MAP_IDX(elevation_map_meta.width, (int)round(index_map(0)), (int)round(index_map(1)));
//
//      int16_t* pt_local_map = &local_elevation_map.data[cell_index];
//      int16_t* pt_global_map = &global_elevation_map.data[cell_index];
//      double*  pt_var = &cell_variance[cell_index];
//
//
//      if(local_map_is_subscribed)
//      {
//          // elevation in current cell in meter
//          double cell_elevation = elevation_map_meta.resolution_z*(*pt_local_map-elevation_map_meta.zero_elevation);
//
//          // store maximum of each cell
//          if(pt_cloud.z > cell_elevation)
//              *pt_local_map = (int16_t)(round(pt_cloud.z/elevation_map_meta.resolution_z) + (int16_t)elevation_map_meta.zero_elevation);
//
//          // filter each cell localy
////            double measurement_variance = sensor_variance*(measurement_distance*measurement_distance);
////            if(*pt_local_map == (int16_t)-elevation_map_meta.zero_elevation)
////            {
////                // unknown cell -> use current measurement
////                *pt_local_map = (int16_t)(round(pt_cloud.z/elevation_map_meta.resolution_z) + (int16_t)elevation_map_meta.zero_elevation);
////                *pt_var = measurement_variance;
////            }
////            else
////            {
////                // fuse cell_elevation with measurement
////                *pt_local_map = (int16_t) (round(((measurement_variance * cell_elevation + *pt_var * pt_cloud.z)/(*pt_var + measurement_variance))/elevation_map_meta.resolution_z) + (int16_t)elevation_map_meta.zero_elevation);
////                *pt_var = (measurement_variance * *pt_var)/(measurement_variance + *pt_var);
////            }
//      }
//
//      if(publish_poseupdate || global_map_is_subscribed)
//      {
//          // fuse new measurements with existing map
//
//          // elevation in current cell in meter
//          double cell_elevation = elevation_map_meta.resolution_z*(*pt_global_map-elevation_map_meta.zero_elevation);
//
//          // measurement variance
//          double measurement_variance = sensor_variance*(measurement_distance*measurement_distance);
//
//          // mahalanobis distance
//          double mahalanobis_distance = sqrt((pt_cloud.z - cell_elevation)*(pt_cloud.z - cell_elevation)/(measurement_variance*measurement_variance));
//
//          if(pt_cloud.z > cell_elevation && (mahalanobis_distance > 5.0))
//          {
//              *pt_global_map = (int16_t)(round(pt_cloud.z/elevation_map_meta.resolution_z) + (int16_t)elevation_map_meta.zero_elevation);
//              *pt_var = measurement_variance;
//              continue;
//          }
//
//          if((pt_cloud.z < cell_elevation) && (mahalanobis_distance > 5.0))
//          {
//              *pt_global_map = (int16_t) (round(((measurement_variance * cell_elevation + *pt_var * pt_cloud.z)/(*pt_var + measurement_variance))/elevation_map_meta.resolution_z) + (int16_t)elevation_map_meta.zero_elevation);
//              //*pt_var = (measurement_variance * *pt_var)/(measurement_variance + *pt_var);
//              *pt_var = measurement_variance;
//              continue;
//          }
//
//          *pt_global_map = (int16_t) (round(((measurement_variance * cell_elevation + *pt_var * pt_cloud.z)/(*pt_var + measurement_variance))/elevation_map_meta.resolution_z) + (int16_t)elevation_map_meta.zero_elevation);
//          *pt_var = (measurement_variance * *pt_var)/(measurement_variance + *pt_var);
//      }
//  }
//
//
//  if(local_map_is_subscribed)
//  {
//      // set the header information on the map
//      local_elevation_map.header.stamp = pointcloud2_sensor_msg->header.stamp;
//      local_elevation_map.header.frame_id = map_frame_id;
//
//      pub_local_map.publish(local_elevation_map);
//  }
//
//  if(global_map_is_subscribed)
//  {
//      // set the header information on the map
//      global_elevation_map.header.stamp = pointcloud2_sensor_msg->header.stamp;
//      global_elevation_map.header.frame_id = map_frame_id;
//
//      pub_global_map.publish(global_elevation_map);
//  }



  return true;
}

bool ElevationMap::publishElevationMap()
{
  if (elevationMapPublisher_.getNumSubscribers () < 1) return false;

  starleth_elevation_msg::ElevationMap elevationMapMessage;

  elevationMapMessage.header.stamp = timeOfLastUpdate_;
  elevationMapMessage.header.frame_id = elevationMapFrameId_;
  elevationMapMessage.resolution = resolution_;
  elevationMapMessage.lengthInX = length_(0);
  elevationMapMessage.lengthInY = length_(1);

  starleth_elevation_msg::matrixEigenToMultiArrayMessage(elevationData_, elevationMapMessage.elevation);
  starleth_elevation_msg::matrixEigenToMultiArrayMessage(varianceData_, elevationMapMessage.variance);
  starleth_elevation_msg::matrixEigenToMultiArrayMessage(varianceDataX_, elevationMapMessage.varianceX);
  starleth_elevation_msg::matrixEigenToMultiArrayMessage(varianceDataY_, elevationMapMessage.varianceY);
  starleth_elevation_msg::matrixEigenToMultiArrayMessage(colorData_, elevationMapMessage.color);

  elevationMapPublisher_.publish(elevationMapMessage);

  ROS_DEBUG("Elevation map has been published.");

  return true;
}

bool ElevationMap::resize(const Eigen::Array2d& length)
{
  // TODO Check if there is a remainder.
  // TODO make this complaint with any format.

  length_ = length;
  int nRows = static_cast<int>(length_(0) / resolution_);
  int nCols = static_cast<int>(length_(1) / resolution_);
  elevationData_.resize(nRows, nCols);
  varianceData_.resize(nRows, nCols);
  varianceDataX_.resize(nRows, nCols);
  varianceDataY_.resize(nRows, nCols);
  colorData_.resize(nRows, nCols);

  ROS_DEBUG_STREAM("Elevation map matrix resized to " << elevationData_.rows() << " rows and "  << elevationData_.cols() << " columns.");
  return true;
}

bool ElevationMap::reset()
{
  elevationData_.setConstant(NAN);
  varianceData_.setConstant(NAN);
  varianceDataX_.setConstant(NAN);
  varianceDataY_.setConstant(NAN);
  colorData_.setConstant(0);
  return true;
}

void ElevationMap::setTimeOfLastUpdate(const ros::Time& timeOfLastUpdate)
{
  timeOfLastUpdate_ = timeOfLastUpdate;
}

} /* namespace starleth_elevation_map */
