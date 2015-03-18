#include "octomap_world/octomap_world.h"

#include <glog/logging.h>
#include <octomap_msgs/conversions.h>
#include <octomap_ros/conversions.h>
#include <pcl/conversions.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/transforms.h>

namespace volumetric_mapping {

// Create a default parameters object and call the other constructor with it.
OctomapWorld::OctomapWorld() : OctomapWorld(OctomapParameters()) {}

// Creates an octomap with the correct parameters.
OctomapWorld::OctomapWorld(const OctomapParameters& params) {
  setOctomapParameters(params);
}

void OctomapWorld::resetMap() {
  CHECK(octree_) << "Octree uninitialized!";
  octree_->clear();
}

void OctomapWorld::setOctomapParameters(const OctomapParameters& params) {
  if (octree_) {
    if (octree_->getResolution() != params.resolution) {
      LOG(WARNING) << "Octomap resolution has changed! Deleting tree!";
      octree_.reset(new octomap::OcTree(params.resolution));
    }
  } else {
    octree_.reset(new octomap::OcTree(params.resolution));
  }

  octree_->setProbHit(params.probability_hit);
  octree_->setProbMiss(params.probability_miss);
  octree_->setClampingThresMin(params.threshold_min);
  octree_->setClampingThresMax(params.threshold_max);

  // Copy over all the parameters for future use (some are not used just for
  // creating the octree).
  params_ = params;
}

void OctomapWorld::insertDisparityImage(
    const Transformation& sensor_to_world,
    const stereo_msgs::DisparityImageConstPtr& disparity) {
  LOG(FATAL) << "TODO!";
}

void OctomapWorld::insertPointcloud(
    const Transformation& sensor_to_world,
    const sensor_msgs::PointCloud2::ConstPtr& cloud_msg) {
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);
  pcl::fromROSMsg(*cloud_msg, *cloud);

  // First, rotate the pointcloud into the world frame.
  pcl::transformPointCloud(*cloud, *cloud,
                           sensor_to_world.getTransformationMatrix());
  // Get the sensor origin in the world frame.
  Eigen::Vector3d sensor_origin_eigen = Eigen::Vector3d::Zero();
  sensor_origin_eigen = sensor_to_world * sensor_origin_eigen;
  octomap::point3d sensor_origin(sensor_origin_eigen.x(),
                                 sensor_origin_eigen.y(),
                                 sensor_origin_eigen.z());

  // Then add all the rays from this pointcloud.
  // We do this as a batch operation - so first get all the keys in a set, then
  // do the update in batch.
  octomap::KeySet free_cells, occupied_cells;
  for (pcl::PointCloud<pcl::PointXYZ>::const_iterator it = cloud->begin();
       it != cloud->end(); ++it) {
    octomap::point3d point(it->x, it->y, it->z);
    // Check if this is within the allowed sensor range.
    if (params_.sensor_max_range < 0.0 ||
        (point - sensor_origin).norm() <= params_.sensor_max_range) {

      // Cast a ray to compute all the free cells.
      octomap::KeyRay key_ray;
      if (octree_->computeRayKeys(sensor_origin, point, key_ray)) {
        free_cells.insert(key_ray.begin(), key_ray.end());
      }
      // Mark endpoing as occupied.
      octomap::OcTreeKey key;
      if (octree_->coordToKeyChecked(point, key)) {
        occupied_cells.insert(key);
      }
    } else {
      // If the ray is longer than the max range, just update free space.
      octomap::point3d new_end =
          sensor_origin +
          (point - sensor_origin).normalized() * params_.sensor_max_range;
      octomap::KeyRay key_ray;
      if (octree_->computeRayKeys(sensor_origin, new_end, key_ray)) {
        free_cells.insert(key_ray.begin(), key_ray.end());
      }
    }
  }

  // Mark free cells.
  for (octomap::KeySet::iterator it = free_cells.begin(),
                                 end = free_cells.end();
       it != end; ++it) {
    if (occupied_cells.find(*it) == occupied_cells.end()) {
      octree_->updateNode(*it, false);
    }
  }

  // Mark occupied cells.
  for (octomap::KeySet::iterator it = occupied_cells.begin(),
                                 end = occupied_cells.end();
       it != end; it++) {
    octree_->updateNode(*it, true);
  }

  octree_->updateInnerOccupancy();
}

OctomapWorld::CellStatus OctomapWorld::getCellStatusBoundingBox(
    const Eigen::Vector3d& point,
    const Eigen::Vector3d& bounding_box_size) const {
  // First case: center point is unknown or occupied. Can just quit.
  // Weird thing: previous implementation does NOT test the bounding box for
  // unknown nodes (in fact, this seems to be like... seriously non-trivial,
  // given the leaf_bbx_interator implementation).
  CellStatus center_status = getCellStatusPoint(point);
  if (center_status != CellStatus::kFree) {
    return center_status;
  }

  // Now we have to iterate over everything in the bounding box.
  Eigen::Vector3d bbx_min_eigen = point - bounding_box_size / 2;
  Eigen::Vector3d bbx_max_eigen = point + bounding_box_size / 2;

  octomap::point3d bbx_min(bbx_min_eigen.x(), bbx_min_eigen.y(),
                           bbx_min_eigen.z());
  octomap::point3d bbx_max(bbx_max_eigen.x(), bbx_max_eigen.y(),
                           bbx_max_eigen.z());

  for (octomap::OcTree::leaf_bbx_iterator
           iter = octree_->begin_leafs_bbx(bbx_min, bbx_max),
           end = octree_->end_leafs_bbx();
       iter != end; ++iter) {
    if (octree_->isNodeOccupied(*iter)) {
      if (params_.filter_speckles && isSpeckleNode(iter.getKey())) {
        continue;
      } else {
        return CellStatus::kOccupied;
      }
    }
  }
  return CellStatus::kFree;
}

OctomapWorld::CellStatus OctomapWorld::getCellStatusPoint(
    const Eigen::Vector3d& point) const {
  octomap::OcTreeNode* node = octree_->search(point.x(), point.y(), point.z());
  if (node == NULL) {
    return CellStatus::kUnknown;
  } else if (octree_->isNodeOccupied(node)) {
    return CellStatus::kOccupied;
  } else {
    return CellStatus::kFree;
  }
}

OctomapWorld::CellStatus OctomapWorld::getLineStatus(
    const Eigen::Vector3d& start, const Eigen::Vector3d& end) const {
  // Get all node keys for this line.
  // This is actually a typedef for a vector of OcTreeKeys.
  octomap::KeyRay key_ray;

  octree_->computeRayKeys(octomap::point3d(start.x(), start.y(), start.z()),
                          octomap::point3d(end.x(), end.y(), end.z()), key_ray);

  // Now check if there are any unknown or occupied nodes in the ray.
  for (octomap::OcTreeKey key : key_ray) {
    octomap::OcTreeNode* node = octree_->search(key);
    if (node == NULL) {
      return CellStatus::kUnknown;
    } else if (octree_->isNodeOccupied(node)) {
      return CellStatus::kOccupied;
    }
  }
  return CellStatus::kFree;
}

OctomapWorld::CellStatus OctomapWorld::getLineStatusBoundingBox(
    const Eigen::Vector3d& start, const Eigen::Vector3d& end,
    const Eigen::Vector3d& bounding_box) const {
  LOG(FATAL) << "TODO. This one is harder.";
  // TODO(helenol): Probably best way would be to get all the coordinates along
  // the line, then make a set of all the OcTreeKeys in all the bounding boxes
  // around the nodes... and then just go through and query once.
}

double OctomapWorld::getResolution() const {
  CHECK(octree_) << "Octree uninitialized!";
  return octree_->getResolution();
}

void OctomapWorld::setLogOddsBoundingBox(const Eigen::Vector3d& position,
                                         const Eigen::Vector3d& bounding_box,
                                         double log_odds_value) {
  CHECK(octree_) << "Octree uninitialized!";

  const bool lazy_eval = true;
  const double resolution = octree_->getResolution();
  const double epsilon = 0.001;  // Small offset to not hit boundary of nodes.

  for (double x_position = position.x() - bounding_box.x() / 2 - epsilon;
       x_position <= position.x() + bounding_box.x() / 2 + epsilon;
       x_position += resolution) {
    for (double y_position = position.y() - bounding_box.y() / 2 - epsilon;
         y_position <= position.y() + bounding_box.y() / 2 + epsilon;
         y_position += resolution) {
      for (double z_position = position.z() - bounding_box.z() / 2 - epsilon;
           z_position <= position.z() + bounding_box.z() / 2 + epsilon;
           z_position += resolution) {
        octomap::point3d point =
            octomap::point3d(x_position, y_position, z_position);
        octree_->setNodeValue(point, log_odds_value, lazy_eval);
      }
    }
  }
  // This is necessary since lazy_eval is set to true.
  octree_->updateInnerOccupancy();
}

bool OctomapWorld::getOctomapBinaryMsg(octomap_msgs::Octomap* msg) const {
  return octomap_msgs::binaryMapToMsg(*octree_, *msg);
}

bool OctomapWorld::getOctomapFullMsg(octomap_msgs::Octomap* msg) const {
  return octomap_msgs::fullMapToMsg(*octree_, *msg);
}

void OctomapWorld::setOctomapFromMsg(const octomap_msgs::Octomap& msg) {
  if (msg.binary) {
    setOctomapFromBinaryMsg(msg);
  } else {
    setOctomapFromFullMsg(msg);
  }
}

void OctomapWorld::setOctomapFromBinaryMsg(const octomap_msgs::Octomap& msg) {
  octree_.reset(octomap_msgs::binaryMsgToMap(msg));
}

void OctomapWorld::setOctomapFromFullMsg(const octomap_msgs::Octomap& msg) {
  octree_.reset(
      dynamic_cast<octomap::OcTree*>(octomap_msgs::fullMsgToMap(msg)));
}

bool OctomapWorld::loadOctomapFromFile(const std::string& filename) {
  if (!octree_) {
    // TODO(helenol): Resolution shouldn't matter... I think. I'm not sure.
    octree_.reset(new octomap::OcTree(0.05));
  }
  return octree_->readBinary(filename);
}

bool OctomapWorld::writeOctomapToFile(const std::string& filename) {
  return octree_->writeBinary(filename);
}

bool OctomapWorld::isSpeckleNode(const octomap::OcTreeKey& key) const {
  octomap::OcTreeKey current_key;
  // Search neighbors in a +/-1 key range cube. If there are neighbors, it's
  // not a speckle.
  bool neighbor_found = false;
  for (current_key[2] = key[2] - 1; current_key[2] <= key[2] + 1;
       ++current_key[2]) {
    for (current_key[1] = key[1] - 1; current_key[1] <= key[1] + 1;
         ++current_key[1]) {
      for (current_key[0] = key[0] - 1; current_key[0] <= key[0] + 1;
           ++current_key[0]) {
        if (current_key != key) {
          octomap::OcTreeNode* node = octree_->search(key);
          if (node && octree_->isNodeOccupied(node)) {
            // we have a neighbor => not a speckle!
            return false;
          }
        }
      }
    }
  }
  return true;
}

}  // namespace volumetric_mapping
