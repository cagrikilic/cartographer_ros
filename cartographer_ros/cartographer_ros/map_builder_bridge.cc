/*
 * Copyright 2016 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cartographer_ros/map_builder_bridge.h"

#include "cartographer_ros/assets_writer.h"
#include "cartographer_ros/msg_conversion.h"
#include "cartographer_ros/occupancy_grid.h"
#include "cartographer_ros_msgs/TrajectorySubmapList.h"

namespace cartographer_ros {

MapBuilderBridge::MapBuilderBridge(
    const NodeOptions& options,
    const std::unordered_set<string>& expected_sensor_ids,
    tf2_ros::Buffer* const tf_buffer)
    : options_(options),
      map_builder_(options.map_builder_options, &constant_data_),
      expected_sensor_ids_(expected_sensor_ids),
      trajectory_id_(map_builder_.AddTrajectoryBuilder(expected_sensor_ids_)),
      tf_bridge_(options_.tracking_frame, options_.lookup_transform_timeout_sec,
                 tf_buffer) {
  sensor_bridge_ = cartographer::common::make_unique<SensorBridge>(
      &tf_bridge_, map_builder_.GetTrajectoryBuilder(trajectory_id_));
}

MapBuilderBridge::~MapBuilderBridge() {
  map_builder_.FinishTrajectory(trajectory_id_);
}

bool MapBuilderBridge::HandleSubmapQuery(
    cartographer_ros_msgs::SubmapQuery::Request& request,
    cartographer_ros_msgs::SubmapQuery::Response& response) {
  cartographer::mapping::proto::SubmapQuery::Response response_proto;
  const std::string error = map_builder_.SubmapToProto(
      request.trajectory_id, request.submap_index, &response_proto);
  if (!error.empty()) {
    LOG(ERROR) << error;
    return false;
  }

  response.submap_version = response_proto.submap_version();
  response.cells.insert(response.cells.begin(), response_proto.cells().begin(),
                        response_proto.cells().end());
  response.width = response_proto.width();
  response.height = response_proto.height();
  response.resolution = response_proto.resolution();
  response.slice_pose = ToGeometryMsgPose(
      cartographer::transform::ToRigid3(response_proto.slice_pose()));
  return true;
}

bool MapBuilderBridge::HandleFinishTrajectory(
    cartographer_ros_msgs::FinishTrajectory::Request& request,
    cartographer_ros_msgs::FinishTrajectory::Response& response) {
  LOG(INFO) << "Finishing trajectory...";

  const int previous_trajectory_id = trajectory_id_;
  trajectory_id_ = map_builder_.AddTrajectoryBuilder(expected_sensor_ids_);
  sensor_bridge_ = cartographer::common::make_unique<SensorBridge>(
      &tf_bridge_, map_builder_.GetTrajectoryBuilder(trajectory_id_));

  map_builder_.FinishTrajectory(previous_trajectory_id);
  map_builder_.sparse_pose_graph()->RunFinalOptimization();

  const auto trajectory_nodes =
      map_builder_.sparse_pose_graph()->GetTrajectoryNodes();
  if (trajectory_nodes.empty()) {
    LOG(WARNING) << "No data collected and no assets will be written.";
  } else {
    LOG(INFO) << "Writing assets...";
    WriteAssets(trajectory_nodes, options_, request.stem);
  }

  LOG(INFO) << "New trajectory started.";
  return true;
}

cartographer_ros_msgs::SubmapList MapBuilderBridge::GetSubmapList() {
  cartographer_ros_msgs::SubmapList submap_list;
  submap_list.header.stamp = ::ros::Time::now();
  submap_list.header.frame_id = options_.map_frame;
  for (int trajectory_id = 0;
       trajectory_id < map_builder_.num_trajectory_builders();
       ++trajectory_id) {
    const cartographer::mapping::Submaps* submaps =
        map_builder_.GetTrajectoryBuilder(trajectory_id)->submaps();
    const std::vector<cartographer::transform::Rigid3d> submap_transforms =
        map_builder_.sparse_pose_graph()->GetSubmapTransforms(*submaps);
    CHECK_EQ(submap_transforms.size(), submaps->size());

    cartographer_ros_msgs::TrajectorySubmapList trajectory_submap_list;
    for (int submap_index = 0; submap_index != submaps->size();
         ++submap_index) {
      cartographer_ros_msgs::SubmapEntry submap_entry;
      submap_entry.submap_version =
          submaps->Get(submap_index)->end_laser_fan_index;
      submap_entry.pose = ToGeometryMsgPose(submap_transforms[submap_index]);
      trajectory_submap_list.submap.push_back(submap_entry);
    }
    submap_list.trajectory.push_back(trajectory_submap_list);
  }
  return submap_list;
}

std::unique_ptr<nav_msgs::OccupancyGrid> MapBuilderBridge::BuildOccupancyGrid() {
  const auto trajectory_nodes =
      map_builder_.sparse_pose_graph()->GetTrajectoryNodes();
  std::unique_ptr<nav_msgs::OccupancyGrid> occupancy_grid;
  if (!trajectory_nodes.empty()) {
    occupancy_grid =
        cartographer::common::make_unique<nav_msgs::OccupancyGrid>();
    cartographer_ros::BuildOccupancyGrid(trajectory_nodes, options_,
                                         occupancy_grid.get());
  }
  return occupancy_grid;
}

}  // namespace cartographer_ros