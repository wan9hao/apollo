/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file
 **/

#include "modules/planning/lattice/trajectory_generation/trajectory_evaluator.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

#include "modules/common/log.h"
#include "modules/planning/common/planning_gflags.h"
#include "modules/planning/constraint_checker/constraint_checker1d.h"
#include "modules/planning/lattice/trajectory1d/piecewise_acceleration_trajectory1d.h"

namespace apollo {
namespace planning {

using Trajectory1d = Curve1d;

TrajectoryEvaluator::TrajectoryEvaluator(
    const std::array<double, 3>& init_s, const PlanningTarget& planning_target,
    const std::vector<std::shared_ptr<Trajectory1d>>& lon_trajectories,
    const std::vector<std::shared_ptr<Trajectory1d>>& lat_trajectories,
    std::shared_ptr<PathTimeGraph> path_time_graph)
    : path_time_graph_(path_time_graph), init_s_(init_s) {
  const double start_time = 0.0;
  const double end_time = FLAGS_trajectory_time_length;
  path_time_intervals_ = path_time_graph_->GetPathBlockingIntervals(
      start_time, end_time, FLAGS_trajectory_time_resolution);

  // if we have a stop point along the reference line,
  // filter out the lon. trajectories that pass the stop point.
  double stop_point = std::numeric_limits<double>::max();
  if (planning_target.has_stop_point()) {
    stop_point = planning_target.stop_point().s();
  }
  for (const auto lon_trajectory : lon_trajectories) {
    double lon_end_s = lon_trajectory->Evaluate(0, end_time);
    if (lon_end_s > stop_point) {
      continue;
    }

    if (!ConstraintChecker1d::IsValidLongitudinalTrajectory(*lon_trajectory)) {
      continue;
    }
    for (const auto lat_trajectory : lat_trajectories) {
      /**
      if (!ConstraintChecker1d::IsValidLateralTrajectory(*lat_trajectory,
                                                         *lon_trajectory)) {
        continue;
      }
      */
      if (!FLAGS_enable_auto_tuning) {
        double cost = Evaluate(planning_target, lon_trajectory, lat_trajectory);
        cost_queue_.push(PairCost({lon_trajectory, lat_trajectory}, cost));
      } else {
        std::vector<double> cost_components;
        double cost = Evaluate(planning_target, lon_trajectory, lat_trajectory,
                               &cost_components);
        cost_queue_with_components_.push(PairCostWithComponents(
            {lon_trajectory, lat_trajectory}, {cost_components, cost}));
      }
    }
  }
  if (!FLAGS_enable_auto_tuning) {
    ADEBUG << "Number of valid 1d trajectory pairs: " << cost_queue_.size();
  } else {
    ADEBUG << "Number of valid 1d trajectory pairs: "
           << cost_queue_with_components_.size();
  }
}

bool TrajectoryEvaluator::has_more_trajectory_pairs() const {
  if (!FLAGS_enable_auto_tuning) {
    return !cost_queue_.empty();
  } else {
    return !cost_queue_with_components_.empty();
  }
}

std::size_t TrajectoryEvaluator::num_of_trajectory_pairs() const {
  if (!FLAGS_enable_auto_tuning) {
    return cost_queue_.size();
  } else {
    return cost_queue_with_components_.size();
  }
}

std::pair<std::shared_ptr<Trajectory1d>, std::shared_ptr<Trajectory1d>>
TrajectoryEvaluator::next_top_trajectory_pair() {
  CHECK(has_more_trajectory_pairs() == true);
  if (!FLAGS_enable_auto_tuning) {
    auto top = cost_queue_.top();
    cost_queue_.pop();
    return top.first;
  } else {
    auto top = cost_queue_with_components_.top();
    cost_queue_with_components_.pop();
    return top.first;
  }
}

double TrajectoryEvaluator::top_trajectory_pair_cost() const {
  if (!FLAGS_enable_auto_tuning) {
    return cost_queue_.top().second;
  } else {
    return cost_queue_with_components_.top().second.second;
  }
}

std::vector<double> TrajectoryEvaluator::top_trajectory_pair_component_cost()
    const {
  CHECK(FLAGS_enable_auto_tuning);
  return cost_queue_with_components_.top().second.first;
}

double TrajectoryEvaluator::Evaluate(
    const PlanningTarget& planning_target,
    const std::shared_ptr<Trajectory1d>& lon_trajectory,
    const std::shared_ptr<Trajectory1d>& lat_trajectory,
    std::vector<double>* cost_components) const {
  // Costs:
  // 1. Cost of missing the objective, e.g., cruise, stop, etc.
  // 2. Cost of logitudinal jerk
  // 3. Cost of logitudinal collision
  // 4. Cost of lateral offsets
  // 5. Cost of lateral comfort

  // Longitudinal costs
  auto reference_s_dot = ComputeLongitudinalGuideVelocity(planning_target);

  double lon_objective_cost = LonObjectiveCost(lon_trajectory, planning_target,
      reference_s_dot);

  double lon_jerk_cost = LonComfortCost(lon_trajectory);

  double lon_collision_cost = LonCollisionCost(lon_trajectory);

  // decides the longitudinal evaluation horizon for lateral trajectories.
  double evaluation_horizon =
      std::min(FLAGS_decision_horizon,
               lon_trajectory->Evaluate(0, lon_trajectory->ParamLength()));
  std::vector<double> s_values;
  for (double s = 0.0; s < evaluation_horizon;
       s += FLAGS_trajectory_space_resolution) {
    s_values.push_back(s);
  }

  // Lateral costs
  double lat_offset_cost = LatOffsetCost(lat_trajectory, s_values);

  double lat_comfort_cost = LatComfortCost(lon_trajectory, lat_trajectory);

  if (cost_components != nullptr) {
    cost_components->push_back(lon_objective_cost);
    cost_components->push_back(lon_jerk_cost);
    cost_components->push_back(lon_collision_cost);
    cost_components->push_back(lat_offset_cost);
  }

  return lon_objective_cost * FLAGS_weight_lon_travel +
         lon_jerk_cost * FLAGS_weight_lon_jerk +
         lon_collision_cost * FLAGS_weight_lon_collision +
         lat_offset_cost * FLAGS_weight_lat_offset +
         lat_comfort_cost * FLAGS_weight_lat_comfort;
}

double TrajectoryEvaluator::LatOffsetCost(
    const std::shared_ptr<Trajectory1d>& lat_trajectory,
    const std::vector<double>& s_values) const {
  double lat_offset_start = lat_trajectory->Evaluate(0, 0.0);
  double cost_sqr_sum = 0.0;
  double cost_abs_sum = 0.0;
  for (const auto& s : s_values) {
    double lat_offset = lat_trajectory->Evaluate(0, s);
    double cost = lat_offset / FLAGS_lat_offset_bound;
    if (lat_offset * lat_offset_start < 0.0) {
      cost_sqr_sum += cost * cost * FLAGS_weight_opposite_side_offset;
      cost_abs_sum += std::abs(cost) * FLAGS_weight_opposite_side_offset;
    } else {
      cost_sqr_sum += cost * cost * FLAGS_weight_same_side_offset;
      cost_abs_sum += std::abs(cost) * FLAGS_weight_same_side_offset;
    }
  }
  return cost_sqr_sum / (cost_abs_sum + FLAGS_lattice_epsilon);
}

double TrajectoryEvaluator::LatComfortCost(
    const std::shared_ptr<Trajectory1d>& lon_trajectory,
    const std::shared_ptr<Trajectory1d>& lat_trajectory) const {
  double max_cost = 0.0;
  for (double t = 0.0; t < FLAGS_trajectory_time_length;
       t += FLAGS_trajectory_time_resolution) {
    double s = lon_trajectory->Evaluate(0, t);
    double s_dot = lon_trajectory->Evaluate(1, t);
    double s_dotdot = lon_trajectory->Evaluate(2, t);
    double l_prime = lat_trajectory->Evaluate(1, s);
    double l_primeprime = lat_trajectory->Evaluate(2, s);
    double cost = l_primeprime * s_dot * s_dot + l_prime * s_dotdot;
    max_cost = std::max(max_cost, std::abs(cost));
  }
  return max_cost;
}

double TrajectoryEvaluator::LonComfortCost(
    const std::shared_ptr<Trajectory1d>& lon_trajectory) const {
  double cost_sqr_sum = 0.0;
  double cost_abs_sum = 0.0;
  for (double t = 0.0; t < FLAGS_trajectory_time_length;
       t += FLAGS_trajectory_time_resolution) {
    double jerk = lon_trajectory->Evaluate(3, t);
    double cost = jerk / FLAGS_longitudinal_jerk_upper_bound;
    cost_sqr_sum += cost * cost;
    cost_abs_sum += std::abs(cost);
  }
  return cost_sqr_sum / (cost_abs_sum + FLAGS_lattice_epsilon);
}

double TrajectoryEvaluator::LonObjectiveCost(
    const std::shared_ptr<Trajectory1d>& lon_trajectory,
    const PlanningTarget& planning_target,
    const std::vector<double>& ref_s_dots) const {
  double t_max = lon_trajectory->ParamLength();
  double dist_s = lon_trajectory->Evaluate(0, t_max)
      - lon_trajectory->Evaluate(0, 0.0);

  double speed_cost_sqr_sum = 0.0;
  double speed_cost_weight_sum = 0.0;
  for (std::size_t i = 0; i < ref_s_dots.size(); ++i) {
    double t = i * FLAGS_trajectory_time_resolution;
    double cost = ref_s_dots[i] - lon_trajectory->Evaluate(1, t);
    speed_cost_sqr_sum += t * t * std::abs(cost);
    speed_cost_weight_sum += t * t;
  }
  double speed_cost =
      speed_cost_sqr_sum / (speed_cost_weight_sum + FLAGS_lattice_epsilon);
  double dist_travelled_cost = 1.0 / (1.0 + dist_s);
  return (speed_cost * FLAGS_weight_target_speed +
          dist_travelled_cost * FLAGS_weight_dist_travelled) /
         (FLAGS_weight_target_speed + FLAGS_weight_dist_travelled);
}

// TODO(all): consider putting pointer of reference_line_info and frame
// while constructing trajectory evaluator
double TrajectoryEvaluator::LonCollisionCost(
    const std::shared_ptr<Trajectory1d>& lon_trajectory) const {
  double cost_sqr_sum = 0.0;
  double cost_abs_sum = 0.0;
  for (std::size_t i = 0; i < path_time_intervals_.size(); ++i) {
    const auto& pt_interval = path_time_intervals_[i];
    if (pt_interval.empty()) {
      continue;
    }
    double t = i * FLAGS_trajectory_time_resolution;
    double traj_s = lon_trajectory->Evaluate(0, t);
    double sigma = FLAGS_lon_collision_cost_std;
    for (const auto& m : pt_interval) {
      double dist = 0.0;
      if (traj_s < m.first - FLAGS_lon_collision_yield_buffer) {
        dist = m.first - FLAGS_lon_collision_yield_buffer - traj_s;
      } else if (traj_s > m.second + FLAGS_lon_collision_overtake_buffer) {
        dist = traj_s - m.second - FLAGS_lon_collision_overtake_buffer;
      }
      double cost = std::exp(-dist * dist / (2.0 * sigma * sigma));

      cost_sqr_sum += cost * cost;
      cost_abs_sum += cost;
    }
  }
  return cost_sqr_sum / (cost_abs_sum + FLAGS_lattice_epsilon);
}

std::vector<double> TrajectoryEvaluator::evaluate_per_lonlat_trajectory(
    const PlanningTarget& planning_target,
    const std::vector<apollo::common::SpeedPoint> st_points,
    const std::vector<apollo::common::FrenetFramePoint> sl_points) {
  std::vector<double> ret;
  return ret;
}

std::vector<double> TrajectoryEvaluator::ComputeLongitudinalGuideVelocity(
    const PlanningTarget& planning_target) const {
  double comfort_a = FLAGS_longitudinal_acceleration_lower_bound *
                     FLAGS_comfort_acceleration_factor;

  double cruise_s_dot = planning_target.cruise_speed();

  ConstantAccelerationTrajectory1d lon_traj(init_s_[0], cruise_s_dot);

  if (!planning_target.has_stop_point()) {
    lon_traj.AppendSegment(0.0, FLAGS_trajectory_time_length);
  } else {
    double stop_a = FLAGS_longitudinal_acceleration_lower_bound;
    double stop_s = planning_target.stop_point().s();
    double dist = stop_s - init_s_[0];
    if (dist > FLAGS_lattice_epsilon) {
      stop_a = -cruise_s_dot * cruise_s_dot * 0.5 / dist;
    }
    if (stop_a > comfort_a) {
      double stop_t = cruise_s_dot / (-comfort_a);
      double stop_dist = cruise_s_dot * stop_t * 0.5;
      double cruise_t = (dist - stop_dist) / cruise_s_dot;
      lon_traj.AppendSegment(0.0, cruise_t);
      lon_traj.AppendSegment(comfort_a, stop_t);
    } else {
      double stop_t = cruise_s_dot / (-stop_a);
      lon_traj.AppendSegment(stop_a, stop_t);
    }
    if (lon_traj.ParamLength() < FLAGS_trajectory_time_length) {
      lon_traj.AppendSegment(
          0.0, FLAGS_trajectory_time_length - lon_traj.ParamLength());
    }
  }

  std::vector<double> reference_s_dot;
  for (double t = 0.0; t < FLAGS_trajectory_time_length;
       t += FLAGS_trajectory_time_resolution) {
    reference_s_dot.push_back(lon_traj.Evaluate(1, t));
  }
  return reference_s_dot;
}

}  // namespace planning
}  // namespace apollo
