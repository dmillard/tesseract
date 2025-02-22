/**
 * @file trajopt_planner.cpp
 * @brief Tesseract ROS Trajopt planner
 *
 * @author Levi Armstrong
 * @date April 18, 2018
 * @version TODO
 * @bug No known bugs
 *
 * @copyright Copyright (c) 2017, Southwest Research Institute
 *
 * @par License
 * Software License Agreement (Apache License)
 * @par
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * @par
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <tesseract_common/macros.h>
TESSERACT_COMMON_IGNORE_WARNINGS_PUSH
#include <jsoncpp/json/json.h>
#include <console_bridge/console.h>
#include <trajopt/plot_callback.hpp>
#include <trajopt/problem_description.hpp>
#include <trajopt_utils/config.hpp>
#include <trajopt_utils/logging.hpp>
#include <trajopt_sco/optimizers.hpp>
#include <trajopt_sco/sco_common.hpp>
TESSERACT_COMMON_IGNORE_WARNINGS_POP

#include <tesseract_motion_planners/trajopt/trajopt_freespace_planner.h>
#include <tesseract_motion_planners/trajopt/trajopt_motion_planner.h>

using namespace trajopt;

namespace tesseract_motion_planners
{
/** @brief Construct a basic planner */
TrajOptFreespacePlanner::TrajOptFreespacePlanner(const std::string& name) : pci_(nullptr), config_(nullptr)
{
  name_ = name;
  // TODO: These should be tied to enumeration ints and returned through an getLastErrorMsg() method
  // Error Status Codes
  status_code_map_ = planner_.getAvailableStatusCodes();
}

bool TrajOptFreespacePlanner::terminate()
{
  CONSOLE_BRIDGE_logWarn("Termination of ongoing optimization is not implemented yet");
  return false;
}

bool TrajOptFreespacePlanner::solve(PlannerResponse& response)
{
  if (!isConfigured())
  {
    response.status_code = -5;
    response.status_description = status_code_map_[response.status_code];
    CONSOLE_BRIDGE_logError("Planner %s is not configured", name_.c_str());
    return false;
  }

  tesseract_motion_planners::PlannerResponse planning_response;

  // Solve problem. Results are stored in the response
  bool success = planner_.solve(planning_response);
  response = std::move(planning_response);
  return success;
}

void TrajOptFreespacePlanner::clear()
{
  request_ = PlannerRequest();
  config_ = nullptr;
  pci_ = nullptr;
}

bool TrajOptFreespacePlanner::isConfigured() const
{
  return pci_ != nullptr && config_ != nullptr && planner_.isConfigured();
}

bool TrajOptFreespacePlanner::setConfiguration(const TrajOptFreespacePlannerConfig& config)
{
  // Check that parameters are valid
  if (config.tesseract_ == nullptr)
  {
    CONSOLE_BRIDGE_logError("In trajopt_freespace_planner: env_ is a required parameter and has not been set");
    return false;
  }

  // -------- Construct the problem ------------
  // -------------------------------------------
  ProblemConstructionInfo pci(config.tesseract_);
  pci.kin = pci.getManipulator(config.manipulator_);

  if (pci.kin == nullptr)
  {
    CONSOLE_BRIDGE_logError("In trajopt_array_planner: manipulator_ does not exist in kin_map_");
    return false;
  }

  // Populate Basic Info
  pci.basic_info.n_steps = config.num_steps_;
  pci.basic_info.manip = config.manipulator_;
  pci.basic_info.start_fixed = false;
  pci.basic_info.use_time = false;

  // Populate Init Info
  pci.init_info.type = config.init_type_;
  if (config.init_type_ == trajopt::InitInfo::GIVEN_TRAJ)
  {
    pci.init_info.data = config.seed_trajectory_;
  }

  // Initial cost/constraint start and end step
  // If the start or end is a fixed joint pose this will be modified
  // so cost and constraints are not added to fixed joint poses
  int start_step = 0;
  int end_step = pci.basic_info.n_steps - 1;

  // Set initial point
  auto start_type = config.start_waypoint_->getType();
  switch (start_type)
  {
    case tesseract_motion_planners::WaypointType::JOINT_WAYPOINT:
    {
      start_step += 1;
      JointWaypoint::Ptr start_position = std::static_pointer_cast<JointWaypoint>(config.start_waypoint_);
      // Add initial joint position constraint
      std::shared_ptr<JointPosTermInfo> jv = std::shared_ptr<JointPosTermInfo>(new JointPosTermInfo);
      Eigen::VectorXd coeffs = start_position->coeffs_;
      if (coeffs.size() != pci.kin->numJoints())
        jv->coeffs = std::vector<double>(pci.kin->numJoints(), 1.0);  // Default value
      else
        jv->coeffs = std::vector<double>(coeffs.data(), coeffs.data() + coeffs.rows() * coeffs.cols());
      jv->targets =
          std::vector<double>(start_position->joint_positions_.data(),
                              start_position->joint_positions_.data() + start_position->joint_positions_.size());
      jv->first_step = 0;
      jv->last_step = 0;
      jv->name = "initial_joint_position";
      jv->term_type = start_position->is_critical_ ? TT_CNT : TT_COST;
      start_position->is_critical_ ? pci.cnt_infos.push_back(jv) : pci.cost_infos.push_back(jv);
      break;
    }
    case tesseract_motion_planners::WaypointType::JOINT_TOLERANCED_WAYPOINT:
    {
      // For a toleranced waypoint we add an inequality term and a smaller equality term. This acts as a "leaky" hinge
      // to keep the problem numerically stable.
      JointTolerancedWaypoint::Ptr start_position =
          std::static_pointer_cast<JointTolerancedWaypoint>(config.start_waypoint_);
      std::shared_ptr<JointPosTermInfo> jv = std::shared_ptr<JointPosTermInfo>(new JointPosTermInfo);
      Eigen::VectorXd coeffs = start_position->coeffs_;
      if (coeffs.size() != pci.kin->numJoints())
        jv->coeffs = std::vector<double>(pci.kin->numJoints(), 1.0);  // Default value
      else
        jv->coeffs = std::vector<double>(coeffs.data(), coeffs.data() + coeffs.rows() * coeffs.cols());
      jv->targets =
          std::vector<double>(start_position->joint_positions_.data(),
                              start_position->joint_positions_.data() + start_position->joint_positions_.size());
      jv->upper_tols =
          std::vector<double>(start_position->upper_tolerance_.data(),
                              start_position->upper_tolerance_.data() + start_position->upper_tolerance_.size());
      jv->lower_tols =
          std::vector<double>(start_position->lower_tolerance_.data(),
                              start_position->lower_tolerance_.data() + start_position->lower_tolerance_.size());
      jv->first_step = 0;
      jv->last_step = 0;
      jv->name = "initial_joint_toleranced_position";
      jv->term_type = start_position->is_critical_ ? TT_CNT : TT_COST;
      start_position->is_critical_ ? pci.cnt_infos.push_back(jv) : pci.cost_infos.push_back(jv);

      // Equality cost with coeffs much smaller than inequality
      std::shared_ptr<JointPosTermInfo> jv_equal = std::shared_ptr<JointPosTermInfo>(new JointPosTermInfo);
      std::vector<double> leaky_coeffs;
      for (auto& ind : jv->coeffs)
        leaky_coeffs.push_back(ind * 0.1);
      jv_equal->coeffs = leaky_coeffs;
      jv_equal->targets =
          std::vector<double>(start_position->joint_positions_.data(),
                              start_position->joint_positions_.data() + start_position->joint_positions_.size());
      jv_equal->first_step = 0;
      jv_equal->last_step = 0;
      jv_equal->name = "initial_joint_toleranced_position_leaky";
      // If this was a CNT, then the inequality tolernce would not do anything
      jv_equal->term_type = TT_COST;
      pci.cost_infos.push_back(jv_equal);
      break;
    }
    case tesseract_motion_planners::WaypointType::CARTESIAN_WAYPOINT:
    {
      CartesianWaypoint::Ptr start_pose = std::static_pointer_cast<CartesianWaypoint>(config.start_waypoint_);
      std::shared_ptr<CartPoseTermInfo> pose = std::shared_ptr<CartPoseTermInfo>(new CartPoseTermInfo);
      pose->term_type = start_pose->is_critical_ ? TT_CNT : TT_COST;
      pose->name = "initial_cartesian_position";
      pose->link = config.link_;
      pose->tcp = config.tcp_;
      pose->timestep = 0;
      pose->xyz = start_pose->getPosition();
      pose->wxyz = start_pose->getOrientation();
      Eigen::VectorXd coeffs = start_pose->coeffs_;
      if (coeffs.size() != 6)
      {
        pose->pos_coeffs = Eigen::Vector3d(10, 10, 10);
        pose->rot_coeffs = Eigen::Vector3d(10, 10, 10);
      }
      else
      {
        pose->pos_coeffs = coeffs.head<3>();
        pose->rot_coeffs = coeffs.tail<3>();
      }
      start_pose->is_critical_ ? pci.cnt_infos.push_back(pose) : pci.cost_infos.push_back(pose);
      break;
    }
  }

  // Set final point
  auto end_type = config.end_waypoint_->getType();
  switch (end_type)
  {
    case tesseract_motion_planners::WaypointType::JOINT_WAYPOINT:
    {
      end_step -= 1;
      JointWaypoint::Ptr end_position = std::static_pointer_cast<JointWaypoint>(config.end_waypoint_);
      // Add initial joint position constraint
      std::shared_ptr<JointPosTermInfo> jv = std::shared_ptr<JointPosTermInfo>(new JointPosTermInfo);
      Eigen::VectorXd coeffs = end_position->coeffs_;
      if (coeffs.size() != pci.kin->numJoints())
        jv->coeffs = std::vector<double>(pci.kin->numJoints(), 1.0);  // Default value
      else
        jv->coeffs = std::vector<double>(coeffs.data(), coeffs.data() + coeffs.rows() * coeffs.cols());
      jv->targets = std::vector<double>(end_position->joint_positions_.data(),
                                        end_position->joint_positions_.data() + end_position->joint_positions_.size());
      jv->first_step = pci.basic_info.n_steps - 1;
      jv->last_step = pci.basic_info.n_steps - 1;
      jv->name = "target_joint_position";
      jv->term_type = end_position->is_critical_ ? TT_CNT : TT_COST;
      end_position->is_critical_ ? pci.cnt_infos.push_back(jv) : pci.cost_infos.push_back(jv);
      break;
    }
    case tesseract_motion_planners::WaypointType::JOINT_TOLERANCED_WAYPOINT:
    {
      // For a toleranced waypoint we add an inequality term and a smaller equality term. This acts as a "leaky" hinge
      // to keep the problem numerically stable.
      JointTolerancedWaypoint::Ptr end_position =
          std::static_pointer_cast<JointTolerancedWaypoint>(config.end_waypoint_);
      std::shared_ptr<JointPosTermInfo> jv = std::shared_ptr<JointPosTermInfo>(new JointPosTermInfo);
      Eigen::VectorXd coeffs = end_position->coeffs_;
      if (coeffs.size() != pci.kin->numJoints())
        jv->coeffs = std::vector<double>(pci.kin->numJoints(), 1.0);  // Default value
      else
        jv->coeffs = std::vector<double>(coeffs.data(), coeffs.data() + coeffs.rows() * coeffs.cols());
      jv->targets = std::vector<double>(end_position->joint_positions_.data(),
                                        end_position->joint_positions_.data() + end_position->joint_positions_.size());
      jv->upper_tols =
          std::vector<double>(end_position->upper_tolerance_.data(),
                              end_position->upper_tolerance_.data() + end_position->upper_tolerance_.size());
      jv->lower_tols =
          std::vector<double>(end_position->lower_tolerance_.data(),
                              end_position->lower_tolerance_.data() + end_position->lower_tolerance_.size());
      jv->first_step = pci.basic_info.n_steps - 1;
      jv->last_step = pci.basic_info.n_steps - 1;
      jv->name = "target_joint_toleranced_position";
      jv->term_type = end_position->is_critical_ ? TT_CNT : TT_COST;
      end_position->is_critical_ ? pci.cnt_infos.push_back(jv) : pci.cost_infos.push_back(jv);

      // Equality cost with coeffs much smaller than inequality
      std::shared_ptr<JointPosTermInfo> jv_equal = std::shared_ptr<JointPosTermInfo>(new JointPosTermInfo);
      std::vector<double> leaky_coeffs;
      for (auto& ind : jv->coeffs)
        leaky_coeffs.push_back(ind * 0.1);
      jv_equal->coeffs = leaky_coeffs;
      jv_equal->targets =
          std::vector<double>(end_position->joint_positions_.data(),
                              end_position->joint_positions_.data() + end_position->joint_positions_.size());
      jv_equal->first_step = pci.basic_info.n_steps - 1;
      jv_equal->last_step = pci.basic_info.n_steps - 1;
      jv_equal->name = "joint_toleranced_position_leaky";
      // If this was a CNT, then the inequality tolernce would not do anything
      jv_equal->term_type = TT_COST;
      pci.cost_infos.push_back(jv_equal);
      break;
    }
    case tesseract_motion_planners::WaypointType::CARTESIAN_WAYPOINT:
    {
      CartesianWaypoint::Ptr end_pose = std::static_pointer_cast<CartesianWaypoint>(config.end_waypoint_);
      std::shared_ptr<CartPoseTermInfo> pose = std::shared_ptr<CartPoseTermInfo>(new CartPoseTermInfo);
      pose->term_type = end_pose->is_critical_ ? TT_CNT : TT_COST;
      pose->name = "target_cartesian_position";
      pose->link = config.link_;
      pose->tcp = config.tcp_;
      pose->timestep = pci.basic_info.n_steps - 1;
      pose->xyz = end_pose->getPosition();
      pose->wxyz = end_pose->getOrientation();
      Eigen::VectorXd coeffs = end_pose->coeffs_;
      if (coeffs.size() != 6)
      {
        pose->pos_coeffs = Eigen::Vector3d(10, 10, 10);
        pose->rot_coeffs = Eigen::Vector3d(10, 10, 10);
      }
      else
      {
        pose->pos_coeffs = coeffs.head<3>();
        pose->rot_coeffs = coeffs.tail<3>();
      }
      end_pose->is_critical_ ? pci.cnt_infos.push_back(pose) : pci.cost_infos.push_back(pose);
      break;
    }
  }

  // Set costs for the rest of the points
  if (config.collision_check_)
  {
    std::shared_ptr<CollisionTermInfo> collision = std::shared_ptr<CollisionTermInfo>(new CollisionTermInfo);
    collision->name = "collision_cost";
    collision->term_type = TT_COST;
    collision->continuous = config.collision_continuous_;
    collision->first_step = start_step;
    collision->last_step = end_step;
    collision->gap = 1;
    collision->info = createSafetyMarginDataVector(pci.basic_info.n_steps, config.collision_safety_margin_, 20);
    pci.cost_infos.push_back(collision);
  }
  if (config.smooth_velocities_)
  {
    std::shared_ptr<JointVelTermInfo> jv = std::shared_ptr<JointVelTermInfo>(new JointVelTermInfo);
    jv->coeffs = std::vector<double>(pci.kin->numJoints(), 5.0);
    jv->targets = std::vector<double>(pci.kin->numJoints(), 0.0);
    jv->first_step = 0;
    jv->last_step = pci.basic_info.n_steps - 1;
    jv->name = "joint_velocity_cost";
    jv->term_type = TT_COST;
    pci.cost_infos.push_back(jv);
  }
  if (config.smooth_accelerations_)
  {
    std::shared_ptr<JointAccTermInfo> ja = std::shared_ptr<JointAccTermInfo>(new JointAccTermInfo);
    ja->coeffs = std::vector<double>(pci.kin->numJoints(), 1.0);
    ja->targets = std::vector<double>(pci.kin->numJoints(), 0.0);
    ja->first_step = 0;
    ja->last_step = pci.basic_info.n_steps - 1;
    ja->name = "joint_accel_cost";
    ja->term_type = TT_COST;
    pci.cost_infos.push_back(ja);
  }
  if (config.smooth_jerks_)
  {
    std::shared_ptr<JointJerkTermInfo> jj = std::shared_ptr<JointJerkTermInfo>(new JointJerkTermInfo);
    jj->coeffs = std::vector<double>(pci.kin->numJoints(), 1.0);
    jj->targets = std::vector<double>(pci.kin->numJoints(), 0.0);
    jj->first_step = 0;
    jj->last_step = pci.basic_info.n_steps - 1;
    jj->name = "joint_jerk_cost";
    jj->term_type = TT_COST;
    pci.cost_infos.push_back(jj);
  }
  // Add configuration cost
  if (config.configuration_->joint_positions_.size() > 0)
  {
    assert(config.configuration_->joint_positions_.size() == pci.kin->numJoints());
    JointWaypoint::ConstPtr joint_waypoint = config.configuration_;
    std::shared_ptr<JointPosTermInfo> jp = std::shared_ptr<JointPosTermInfo>(new JointPosTermInfo);
    Eigen::VectorXd coeffs = joint_waypoint->coeffs_;
    if (coeffs.size() != pci.kin->numJoints())
      jp->coeffs = std::vector<double>(pci.kin->numJoints(), 0.1);  // Default value
    else
      jp->coeffs = std::vector<double>(coeffs.data(), coeffs.data() + coeffs.rows() * coeffs.cols());
    jp->targets =
        std::vector<double>(joint_waypoint->joint_positions_.data(),
                            joint_waypoint->joint_positions_.data() + joint_waypoint->joint_positions_.size());
    jp->first_step = start_step;
    jp->last_step = end_step;
    jp->name = "configuration_cost";
    jp->term_type = TT_COST;
    pci.cost_infos.push_back(jp);
  }

  pci_ = std::make_shared<trajopt::ProblemConstructionInfo>(pci);
  config_ = std::make_shared<TrajOptFreespacePlannerConfig>(config);

  trajopt::TrajOptProb::Ptr prob = ConstructProblem(*pci_);
  tesseract_motion_planners::TrajOptPlannerConfig config_planner(prob);
  config_planner.params = config_->params_;
  config_planner.callbacks = config_->callbacks_;

  planner_.clear();
  planner_.setConfiguration(config_planner);

  return true;
}

}  // namespace tesseract_motion_planners
