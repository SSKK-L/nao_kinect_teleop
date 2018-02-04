/***
 * Copyright 2018
 * J.Avalos, O.Ramosx
 * Universidad de Ingenieria y Tecnologia - UTEC
 *
 * This file is part of nao_kinect_teleop.
 * nao_kinect_teleop is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 * nao_kinect_teleop is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details. You should
 * have received a copy of the GNU Lesser General Public License along
 * with nao_kinect_teleop. If not, see <http://www.gnu.org/licenses/>.
 */


#include <fstream>
#include <ros/ros.h>
#include <ros/package.h>

#include <oscr/oscr.hpp>

#include <nao_kinect_teleop/joint-state-pub.hpp>
#include <nao_kinect_teleop/markers.hpp>
#include <nao_kinect_teleop/kinect-arm-points.hpp>


int main(int argc, char **argv)
{
  // Load the urdf model
  // --------------------------------------------------------------------
  // The robot is assumed to be fixed on the ground
  bool has_floating_base = false;
  // Path to the robot URDF (reduced with only 26 dofs)
  std::string model_pkg = ros::package::getPath("nao_kinect_teleop");
  std::string model_name = model_pkg + "/urdf/naoV40red.urdf";
  // Load the robot model (use RobotModelPin or RobotModelRbdl)
  oscr::RobotModel* robot = new oscr::RobotModelRbdl(model_name,
                                                     has_floating_base);
  // --------------------------------------------------------------------

  // Information about joints and their limits
  std::vector<std::string> jnames = robot->jointNames();
  std::map<std::string, unsigned int> mlink = robot->mapLinkNamesIDs();
  // Joint limits (only needed for WQP and HQP)
  Eigen::VectorXd qmin, qmax, dqmax;
  qmin = robot->jointMinAngularLimits();
  qmax = robot->jointMaxAngularLimits();
  dqmax = robot->jointVelocityLimits();

  // Initialize ROS
  ros::init(argc, argv, "naoTeleopArmsRviz");
  ros::NodeHandle nh;

  // Initialize publishers and subscribers
  // --------------------------------------------------------------------
  // * Publisher of  Joint States (for rviz)
  JointStatePub jstate_pub(nh, robot->ndof(), has_floating_base);
  jstate_pub.setJointNames(jnames);
  // * Subscriber to kinect messages
  KinectArmPoints kpoints;
  ros::Subscriber sub = nh.subscribe("kinect_points", 1000,
                                     &KinectArmPoints::readKinectPoints,
                                     &kpoints);

  // Assign the initial joint configuration
  Eigen::VectorXd q(robot->ndof());
  q << 0.0, 0.0,
    0.0, 0.0, -0.1, 0.3, -0.2, 0.0,
    1.15,  0.10, -1.4, -0.79, 0.0, 0.0,
    0.0, 0.0, -0.1, 0.3, -0.2, 0.0,
    1.15, -0.10,  1.4,  0.79, 0.0, 0.0;
  robot->updateJointConfig(q);
  // Publish the initial configuration
  jstate_pub.publish(q);

  // Inverse Kinematics tasks
  // --------------------------------------------------------------------
  oscr::KineTask* taskrh;
  oscr::KineTask* tasklh;
  oscr::KineTask* taskre;
  oscr::KineTask* taskle;
  taskrh = new oscr::KineTaskPose(robot, mlink["r_gripper"], "position");
  taskrh->setGain(10.0);
  tasklh = new oscr::KineTaskPose(robot, mlink["l_gripper"], "position");
  tasklh->setGain(10.0);
  taskre = new oscr::KineTaskPose(robot, mlink["RElbow"], "position");
  taskre->setGain(10.0);
  taskle = new oscr::KineTaskPose(robot, mlink["LElbow"], "position");
  taskle->setGain(10.0);

  // Operational-Space Inverse Kinematics (OSIK) solver
  // --------------------------------------------------------------------
  // Sampling time
  unsigned int f = 30;
  double dt = static_cast<double>(1.0/f);
  // Solver (WQP, HQP or NS)
  oscr::OSIKSolverWQP solver(robot, q, dt);
  solver.setJointLimits(qmin, qmax, dqmax);
  // Add tasks to the solver
  solver.pushTask(taskrh);
  solver.pushTask(tasklh);
  solver.pushTask(taskre);
  solver.pushTask(taskle);

  // Ball markers for the human skeleton
  std::vector<BallMarker*> sk_markers;
  sk_markers.resize(6);
  for (unsigned int i=0; i<sk_markers.size(); ++i)
    sk_markers.at(i) = new BallMarker(nh, RED);
  // Ball markers for the retargeted (nao) skeleton
  std::vector<BallMarker*> nao_markers;
  nao_markers.resize(6);
  for (unsigned int i=0; i<nao_markers.size(); ++i)
    nao_markers.at(i) = new BallMarker(nh, GREEN);

  // TaskMarkers markers(nh);
  // markers.add(tasklh);
  // markers.add(taskrh);

  // Nao lengths
  double Lnao_upperarm = 0.108;  // From shoulder to elbow
  double Lnao_forearm  = 0.111;  // From elbow to hand

  // Vectors for positions/poses
  Eigen::VectorXd p_rshoulder(3), p_relbow(3), p_rwrist(3);
  Eigen::VectorXd p_lshoulder(3), p_lelbow(3), p_lwrist(3);
  Eigen::VectorXd pskel(3);
  // Vector for the desired joint configuration
  Eigen::VectorXd qdes;
  // Storage for points
  std::vector< std::vector<double> > P;
  P.resize(6);
  // Allocate space for the points
  for (unsigned int i=0; i<P.size(); ++i)
    P[i].resize(3);

  ros::Rate rate(f); // Hz
  while(ros::ok())
  {
    unsigned int n_kinect_points = kpoints.getPoints()->body.size();

    // Only work when there are skeleton points from the Kinect
    if (n_kinect_points > 0)
    {
      // Show the body markers corresponding to the human skeleton
      for (unsigned k=0; k<sk_markers.size(); ++k)
      {
        pskel <<
          kpoints.getPoints()->body[k].x,
          kpoints.getPoints()->body[k].y,
          kpoints.getPoints()->body[k].z;
        sk_markers.at(k)->setPose(pskel);
      }

      // ------------------------------------------------------------------
      // Right arm
      // ------------------------------------------------------------------
      // 0=shoulder;   1=elbow;    2=hand
      // Get the positions with respect to the right shoulder
      for (unsigned int k=0; k<(P.size()/2); ++k)
      {
        // The shoulder is taken as the origin: P[0][k] := (0, 0, 0)
        P[k][0] = (kpoints.getPoints()->body[k].x)-(kpoints.getPoints()->body[0].x);
        P[k][1] = (kpoints.getPoints()->body[k].y)-(kpoints.getPoints()->body[0].y);
        P[k][2] = (kpoints.getPoints()->body[k].z)-(kpoints.getPoints()->body[0].z);
      }
      // Length from right shoulder to right elbow
      double Lskel_rupperarm = sqrt( pow(P[1][0],2.0) +
                                     pow(P[1][1],2.0) +
                                     pow(P[1][2],2.0));
      // Length from right elbow to right hand
      double Lskel_rforearm= sqrt(pow(P[2][0] - P[1][0], 2.0) +
                                  pow(P[2][1] - P[1][1], 2.0) +
                                  pow(P[2][2] - P[1][2], 2.0));
      // Ratio: (Nao limbs)/(human skeleton limbs)
      double Q1 = Lnao_upperarm / Lskel_rupperarm;
      double Q2 = Lnao_forearm / Lskel_rforearm;
      // Nao right shoulder position
      P[0][0] = 0.00;
      P[0][1] = -0.098;
      P[0][2] = 0.100;
      // Redefinimos P2
      P[2][0] = Q2*(P[2][0] - P[1][0]);
      P[2][1] = Q2*(P[2][1] - P[1][1]);
      P[2][2] = Q2*(P[2][2] - P[1][2]);
      // Redefinimos P1
      P[1][0] = P[0][0]+Q1*P[1][0];
      P[1][1] = P[0][1]+Q1*P[1][1];
      P[1][2] = P[0][2]+Q1*P[1][2];

      P[2][0] = P[2][0]+P[1][0];
      P[2][1] = P[2][1]+P[1][1];
      P[2][2] = P[2][2]+P[1][2];

      // ------------------------------------------------------------------
      // Left arm
      // ------------------------------------------------------------------
      // 3=shoulder;   4=elbow;    5=hand
      // Get the positions with respect to the left shoulder
      for (unsigned k=(P.size()/2);k<P.size();k++)
      {
        P[k][0] = (kpoints.getPoints()->body[k].x)-(kpoints.getPoints()->body[3].x);
        P[k][1] = (kpoints.getPoints()->body[k].y)-(kpoints.getPoints()->body[3].y);
        P[k][2] = (kpoints.getPoints()->body[k].z)-(kpoints.getPoints()->body[3].z);
      }
      // Length from left shoulder to left elbow
      double Lskel_lupperarm = sqrt(pow(P[4][0], 2.0) +
                                    pow(P[4][1], 2.0) +
                                    pow(P[4][2], 2.0));
      // Length from left elbow to left hand
      double Lskel_lforearm = sqrt(pow(P[5][0] - P[4][0], 2.0) +
                                   pow(P[5][1] - P[4][1], 2.0) +
                                   pow(P[5][2] - P[4][2], 2.0));

      double Q3 = Lnao_upperarm / Lskel_lupperarm;
      double Q4 = Lnao_forearm / Lskel_lforearm;
      // Nao left shoulder position
      P[3][0] = 0.00;
      P[3][1] = 0.098;
      P[3][2] = 0.100;
      //Redefinimos P5
      P[5][0] = Q4*(P[5][0] - P[4][0]);
      P[5][1] = Q4*(P[5][1] - P[4][1]);
      P[5][2] = Q4*(P[5][2] - P[4][2]);
      //Redfinimos P4
      P[4][0] = P[3][0]+Q3*P[4][0];
      P[4][1] = P[3][1]+Q3*P[4][1];
      P[4][2] = P[3][2]+Q3*P[4][2];

      P[5][0] = P[5][0]+P[4][0];
      P[5][1] = P[5][1]+P[4][1];
      P[5][2] = P[5][2]+P[4][2];
      //#######################################################

      // Right shoulder
      p_rshoulder << P[0][0], P[0][1], P[0][2];
      // Right elbow
      p_relbow << P[1][0], P[1][1], P[1][2];
      // Right hand
      p_rwrist << P[2][0], P[2][1], P[2][2];
      // Show markers for the right arm
      nao_markers.at(0)->setPose(p_relbow);
      nao_markers.at(1)->setPose(p_rwrist);
      nao_markers.at(2)->setPose(p_rshoulder);

      // Left shoulder
      p_lshoulder << P[3][0], P[3][1], P[3][2];
      // Left elbow
      p_lelbow << P[4][0], P[4][1], P[4][2];
      // Left hand
      p_lwrist << P[5][0], P[5][1], P[5][2];
      // Show markers for the left arm
      nao_markers.at(3)->setPose(p_lelbow);
      nao_markers.at(4)->setPose(p_lwrist);
      nao_markers.at(5)->setPose(p_lshoulder);

      // Set the desired positions for the tasks
      taskle->setDesiredValue(p_lelbow);
      tasklh->setDesiredValue(p_lwrist);
      taskre->setDesiredValue(p_relbow);
      taskrh->setDesiredValue(p_rwrist);

      solver.getPositionControl(q, qdes);
      robot->updateJointConfig(q);
      jstate_pub.publish(robot->getJointConfig());
      // sk_markers.update();
      // fileq << qdes.transpose() << std::endl;
      q = qdes;
    }
    else
    {
      std::cout << "No skeleton points from Kinect found (" << n_kinect_points
                << " points)" << std::endl;
    }
    ros::spinOnce();
    rate.sleep();
  }

  return 0;
}

