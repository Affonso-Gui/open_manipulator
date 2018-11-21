﻿/*******************************************************************************
* Copyright 2018 ROBOTIS CO., LTD.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

/* Authors: Darby Lim, Hye-Jong KIM, Ryan Shim, Yong-Ho Na */

#include "open_manipulator_controller/open_manipulator_controller.h"

using namespace open_manipulator_controller;

OM_CONTROLLER::OM_CONTROLLER()
    :node_handle_(""),
     priv_node_handle_("~"),
     tool_ctrl_flag_(NONE),
     timer_thread_flag_(false),
     using_platform_(false),
     tool_position_(0.0)
{
  robot_name_             = priv_node_handle_.param<std::string>("robot_name", "open_manipulator");
  std::string usb_port    = priv_node_handle_.param<std::string>("usb_port", "/dev/ttyUSB0");
  std::string baud_rate   = priv_node_handle_.param<std::string>("baud_rate", "1000000");

  using_platform_ = priv_node_handle_.param<bool>("using_platform", false);

  initPublisher();
  initSubscriber();

  chain_.initManipulator(using_platform_, usb_port, baud_rate);

  setTimerThread();
  RM_LOG::INFO("Successed to OpenManipulator initialization");
}

OM_CONTROLLER::~OM_CONTROLLER()
{
  timer_thread_flag_ = false;
  usleep(10 * 1000); // 10ms
  RM_LOG::INFO("Shutdown the OpenManipulator");
  chain_.allActuatorDisable();
  ros::shutdown();
}

void OM_CONTROLLER::setTimerThread()
{
  int error;
  struct sched_param param;
  pthread_attr_t attr;
  pthread_attr_init(&attr);

  error = pthread_attr_setschedpolicy(&attr, SCHED_RR);
  if (error != 0)
    RM_LOG::ERROR("pthread_attr_setschedpolicy error = ", (double)error);
  error = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  if (error != 0)
    RM_LOG::ERROR("pthread_attr_setinheritsched error = ", (double)error);

  memset(&param, 0, sizeof(param));
  param.sched_priority = 31;    // RT
  error = pthread_attr_setschedparam(&attr, &param);
  if (error != 0)
    RM_LOG::ERROR("pthread_attr_setschedparam error = ", (double)error);

  // create and start the thread
  if ((error = pthread_create(&this->timer_thread_, /*&attr*/NULL, this->timerThread, this)) != 0)
  {
    RM_LOG::ERROR("Creating timer thread failed!!", (double)error);
    exit(-1);
  }
  timer_thread_flag_ = true;
  RM_LOG::INFO("Start the OpenManipulator control thread");
}


void *OM_CONTROLLER::timerThread(void *param)
{
  OM_CONTROLLER *controller = (OM_CONTROLLER *) param;
  static struct timespec next_time;
  static struct timespec curr_time;

  clock_gettime(CLOCK_MONOTONIC, &next_time);

  while(controller->timer_thread_flag_)
  {
    next_time.tv_sec += (next_time.tv_nsec + ACTUATOR_CONTROL_TIME_MSEC * 1000000) / 1000000000;
    next_time.tv_nsec = (next_time.tv_nsec + ACTUATOR_CONTROL_TIME_MSEC * 1000000) % 1000000000;

    double time = next_time.tv_sec + (next_time.tv_nsec*0.000000001);
    controller->process(time);

    clock_gettime(CLOCK_MONOTONIC, &curr_time);

    /////
    double delta_nsec = (next_time.tv_sec - curr_time.tv_sec) + (next_time.tv_nsec - curr_time.tv_nsec)*0.000000001;
    //RM_LOG::INFO("control time : ", ACTUATOR_CONTROL_TIME - delta_nsec);
    if(delta_nsec < 0.0)
    {
      RM_LOG::WARN("control time :", ACTUATOR_CONTROL_TIME - delta_nsec);
      next_time = curr_time;
    }
    else
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_time, NULL);
    /////
  }

  return 0;
}

void OM_CONTROLLER::initPublisher()
{
  // msg publisher
  chain_kinematics_pose_pub_  = node_handle_.advertise<open_manipulator_msgs::KinematicsPose>(robot_name_ + "/kinematics_pose", 10);
  if(using_platform_)
    chain_joint_states_pub_  = node_handle_.advertise<sensor_msgs::JointState>(robot_name_ + "/joint_states", 10);
  else
  {
    chain_joint_states_to_gazebo_pub_[0] = node_handle_.advertise<std_msgs::Float64>(robot_name_ + "/joint1_position/command", 10);
    chain_joint_states_to_gazebo_pub_[1] = node_handle_.advertise<std_msgs::Float64>(robot_name_ + "/joint2_position/command", 10);
    chain_joint_states_to_gazebo_pub_[2] = node_handle_.advertise<std_msgs::Float64>(robot_name_ + "/joint3_position/command", 10);
    chain_joint_states_to_gazebo_pub_[3] = node_handle_.advertise<std_msgs::Float64>(robot_name_ + "/joint4_position/command", 10);
    chain_gripper_states_to_gazebo_pub_[0] = node_handle_.advertise<std_msgs::Float64>(robot_name_ + "/grip_joint_position/command", 10);
    chain_gripper_states_to_gazebo_pub_[1] = node_handle_.advertise<std_msgs::Float64>(robot_name_ + "/grip_joint_sub_position/command", 10);
  }
}
void OM_CONTROLLER::initSubscriber()
{
  // service server
  goal_joint_space_path_server_ = node_handle_.advertiseService(robot_name_ + "/goal_joint_space_path", &OM_CONTROLLER::goalJointSpacePathCallback, this);
  goal_task_space_path_server_ = node_handle_.advertiseService(robot_name_ + "/goal_task_space_path", &OM_CONTROLLER::goalTaskSpacePathCallback, this);
  goal_joint_space_path_to_present_server_ = node_handle_.advertiseService(robot_name_ + "/goal_joint_space_path_to_present", &OM_CONTROLLER::goalJointSpacePathToPresentCallback, this);
  goal_task_space_path_to_present_server_ = node_handle_.advertiseService(robot_name_ + "/goal_task_space_path_to_present", &OM_CONTROLLER::goalTaskSpacePathToPresentCallback, this);
  goal_tool_control_server_ = node_handle_.advertiseService(robot_name_ + "/goal_tool_control", &OM_CONTROLLER::goalToolControlCallback, this);
  goal_tool_control_to_present_server_ = node_handle_.advertiseService(robot_name_ + "/goal_tool_control_to_present", &OM_CONTROLLER::goalToolControlToPresentCallback, this);
  toggle_torque_server_ = node_handle_.advertiseService(robot_name_ + "/toggle_torque", &OM_CONTROLLER::toggleTorqueCallback, this);
}

bool OM_CONTROLLER::goalJointSpacePathCallback(open_manipulator_msgs::SetJointPosition::Request  &req,
                                               open_manipulator_msgs::SetJointPosition::Response &res)
{
  std::vector <double> target_angle;

  for(int i = 0; i < req.joint_position.joint_name.size(); i ++)
    target_angle.push_back(req.joint_position.position.at(i));

  chain_.jointTrajectoryMove(target_angle, req.path_time);

  res.isPlanned = true;
  return true;
}
bool OM_CONTROLLER::goalTaskSpacePathCallback(open_manipulator_msgs::SetKinematicsPose::Request  &req,
                                              open_manipulator_msgs::SetKinematicsPose::Response &res)
{
  Pose target_pose;
  target_pose.position[0] = req.kinematics_pose.pose.position.x;
  target_pose.position[1] = req.kinematics_pose.pose.position.y;
  target_pose.position[2] = req.kinematics_pose.pose.position.z;
  Eigen::Vector3d a;
  RM_LOG::PRINT_VECTOR(a);
  chain_.taskTrajectoryMove(TOOL, target_pose.position, req.path_time);

  res.isPlanned = true;
  return true;
}

bool OM_CONTROLLER::goalJointSpacePathToPresentCallback(open_manipulator_msgs::SetJointPosition::Request  &req,
                                                        open_manipulator_msgs::SetJointPosition::Response &res)
{
  std::vector <double> target_angle;

  for(int i = 0; i < req.joint_position.joint_name.size(); i ++)
    target_angle.push_back(req.joint_position.position.at(i));

  chain_.jointTrajectoryMoveToPresentValue(target_angle, req.path_time);

  res.isPlanned = true;
  return true;
}
bool OM_CONTROLLER::goalTaskSpacePathToPresentCallback(open_manipulator_msgs::SetKinematicsPose::Request  &req,
                                                      open_manipulator_msgs::SetKinematicsPose::Response &res)
{
  Pose target_pose;
  target_pose.position[0] = req.kinematics_pose.pose.position.x;
  target_pose.position[1] = req.kinematics_pose.pose.position.y;
  target_pose.position[2] = req.kinematics_pose.pose.position.z;

  chain_.taskTrajectoryMoveToPresentPosition(TOOL, target_pose.position, req.path_time);

  res.isPlanned = true;
  return true;
}
bool OM_CONTROLLER::goalToolControlCallback(open_manipulator_msgs::SetJointPosition::Request  &req,
                                            open_manipulator_msgs::SetJointPosition::Response &res)
{
  tool_position_ = req.joint_position.position.at(0);
  tool_ctrl_flag_ = TOOL_MOVE;

  res.isPlanned = true;
  return true;
}
bool OM_CONTROLLER::goalToolControlToPresentCallback(open_manipulator_msgs::SetJointPosition::Request  &req,
                                                     open_manipulator_msgs::SetJointPosition::Response &res)
{
  tool_position_ = req.joint_position.position.at(0);
  tool_ctrl_flag_ = TOOL_MOVE_TO_PRESENT;

  res.isPlanned = true;
  return true;
}
bool OM_CONTROLLER::toggleTorqueCallback(std_srvs::Trigger::Request  &req,
                                         std_srvs::Trigger::Response &res)
{
  if(chain_.isEnabled())
  {
    chain_.allActuatorDisable();
    res.message = "Torque Disabled";
  }
  else
  {
    chain_.allActuatorEnable();
    res.message = "Torque Enabled";
  }
  res.success = true;
  return true;
}

void OM_CONTROLLER::publishKinematicsPose()
{
  open_manipulator_msgs::KinematicsPose msg;

  Vector3d position = chain_.getManipulator()->getComponentPositionToWorld(TOOL);
  msg.pose.position.x = position[0];
  msg.pose.position.y = position[1];
  msg.pose.position.z = position[2];
  chain_kinematics_pose_pub_.publish(msg);
}

void OM_CONTROLLER::publishJointStates()
{
  if(using_platform_)
  {
    sensor_msgs::JointState msg;
    msg.header.stamp = ros::Time::now();
    std::vector<double> position, velocity, effort;
    chain_.getManipulator()->getAllActiveJointValue(&position, &velocity, &effort);
    double tool_value = chain_.getManipulator()->getToolValue(TOOL);
    msg.name.push_back("joint1");           msg.position.push_back(position.at(0));
                                            msg.velocity.push_back(velocity.at(0));
                                            msg.effort.push_back(effort.at(0));

    msg.name.push_back("joint2");           msg.position.push_back(position.at(1));
                                            msg.velocity.push_back(velocity.at(1));
                                            msg.effort.push_back(effort.at(1));

    msg.name.push_back("joint3");           msg.position.push_back(position.at(2));
                                            msg.velocity.push_back(velocity.at(2));
                                            msg.effort.push_back(effort.at(2));

    msg.name.push_back("joint4");           msg.position.push_back(position.at(3));
                                            msg.velocity.push_back(velocity.at(3));
                                            msg.effort.push_back(effort.at(3));

    msg.name.push_back("grip_joint");       msg.position.push_back(tool_value);
                                            msg.velocity.push_back(0.0);
                                            msg.effort.push_back(0.0);

    msg.name.push_back("grip_joint_sub");   msg.position.push_back(tool_value);
                                            msg.velocity.push_back(0.0);
                                            msg.effort.push_back(0.0);
    chain_joint_states_pub_.publish(msg);
  }
  else // gazebo
  {
    std::vector<double> value = chain_.getManipulator()->getAllActiveJointValue();
    for(int i = 0; i < value.size(); i ++)
    {
      std_msgs::Float64 msg;
      msg.data = value.at(i);
      chain_joint_states_to_gazebo_pub_[i].publish(msg);
    }
    double tool_value = chain_.getManipulator()->getToolGoalValue(TOOL);
    for(int i = 0; i < 2; i ++)
    {
      std_msgs::Float64 msg;
      msg.data = tool_value;
      chain_gripper_states_to_gazebo_pub_[i].publish(msg);
    }
  }
}


void OM_CONTROLLER::process(double time)
{
  chain_.chainProcess(time);

  switch(tool_ctrl_flag_)
  {
    case TOOL_MOVE:
      chain_.toolMove(TOOL, tool_position_);
      tool_ctrl_flag_ = NONE;
      break;
    case TOOL_MOVE_TO_PRESENT:
      chain_.toolMoveToPresentValue(TOOL, tool_position_);
      tool_ctrl_flag_ = NONE;
      break;
  }
}

int main(int argc, char **argv)
{
  // Init ROS node
  ros::init(argc, argv, "open_manipulator_controller");

  OM_CONTROLLER om_controller;
  ros::Rate loop_rate(ITERATION_FREQUENCY);

  while (ros::ok())
  {
    om_controller.publishJointStates();
    om_controller.publishKinematicsPose();
    ros::spinOnce();
    loop_rate.sleep();
  }

  return 0;
}
