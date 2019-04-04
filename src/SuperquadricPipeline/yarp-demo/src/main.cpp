/******************************************************************************
 *                                                                            *
 * Copyright (C) 2018 Fondazione Istituto Italiano di Tecnologia (IIT)        *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

/**
 * @file main.cpp
 * @authors: Giulia Vezzani <giulia.vezzani@iit.it>
 */

 #include <cstdlib>
 #include <string>
 #include <vector>
 #include <cmath>
 #include <algorithm>
 #include <memory>

 #include <yarp/os/all.h>
 #include <yarp/sig/all.h>
 #include <yarp/math/Math.h>
 #include <yarp/eigen/Eigen.h>
 #include <yarp/dev/Drivers.h>
 #include <yarp/dev/CartesianControl.h>
 #include <yarp/dev/PolyDriver.h>

 #include <iCub/ctrl/clustering.h>

 #include <SuperquadricLibModel/superquadricEstimator.h>
 #include <SuperquadricLibVis/visRenderer.h>
 #include <SuperquadricLibGrasp/graspComputation.h>
 #include "src/SuperquadricPipelineDemo_IDL.h"

 using namespace std;
 using namespace SuperqModel;
 using namespace SuperqVis;
 using namespace SuperqGrasp;

 using namespace yarp::os;
 using namespace yarp::sig;
 using namespace yarp::dev;
 using namespace yarp::math;
 using namespace yarp::eigen;
 using namespace iCub::ctrl;

 /****************************************************************/
enum class WhichHand
{
    HAND_RIGHT,
    HAND_LEFT,
    BOTH
};

/****************************************************************/
string prettyError(const char* func_name, const string &message)
{
    //  Nice formatting for errors
    stringstream error;
    error << "[" << func_name << "] " << message;
    return error.str();
};

 /****************************************************************/
class SuperquadricPipelineDemo : public RFModule, SuperquadricPipelineDemo_IDL
{
    string moduleName;

    RpcClient point_cloud_rpc;
    RpcClient action_render_rpc;
    RpcClient reach_calib_rpc;
    RpcClient table_calib_rpc;
    RpcClient sfm_rpc;

    // Connection to image
    BufferedPort<ImageOf<PixelRgb>> img_in;

    RpcServer user_rpc;

    string object_class;
    vector<string> classes;

    bool closing;

    string robot;
    WhichHand grasping_hand;

    PolyDriver left_arm_client, right_arm_client;
    ICartesianControl *icart_right, *icart_left;

    Matrix grasper_specific_transform_right;
    Matrix grasper_specific_transform_left;
    Vector grasper_approach_parameters_right;
    Vector grasper_approach_parameters_left;

    //  visualization parameters
    int x, y, h, w;
    bool fixate_object;

    // Image crop
    int u_i,v_i, u_f, v_f;

    string best_hand;
    bool single_superq;
    int best_pose_1, best_pose_2;

    // PointCloud filtering
    double radius;
    int minpts;

    // Superquadric-lib objects
    SuperqModel::PointCloud point_cloud;
    vector<Superquadric> superqs;
    GraspResults grasp_res_hand1, grasp_res_hand2;
    SuperqEstimatorApp estim;
    GraspEstimatorApp grasp_estim;
    Visualizer vis;

    Eigen::Vector4d plane;
    Eigen::Vector3d displacement;
    Eigen::VectorXd hand;
    Eigen::MatrixXd bounds_right, bounds_left;
    Eigen::MatrixXd bounds_constr_right, bounds_constr_left;

   /****************************************************************/
   bool configure(ResourceFinder &rf) override
    {
        moduleName = rf.check("name", Value("superquadric-lib-demo")).toString();
        if(!rf.check("robot"))
        {
            robot = (rf.check("sim")? "icubSim" : "icub");
        }
        else
        {
            robot = rf.find("robot").asString();
        }

        yInfo() << "Opening module for connection with robot" << robot;

        string control_arms = rf.check("control-arms", Value("both")).toString();

        x = rf.check("x", Value(0)).asInt();
        y = rf.check("y", Value(0)).asInt();
        w = rf.check("width", Value(600)).asInt();
        h = rf.check("height", Value(600)).asInt();

        u_i = rf.check("u_i", Value(50)).asInt();
        v_i = rf.check("v_i", Value(20)).asInt();
        u_f = rf.check("u_f", Value(280)).asInt();
        v_f = rf.check("v_f", Value(190)).asInt();

        vis.setPosition(x,y);
        vis.setSize(w,h);

        Vector grasp_specific_translation(3, 0.0);
        Vector grasp_specific_orientation(4, 0.0);
        Bottle *list = rf.find("grasp_trsfm_right").asList();
        bool valid_grasp_specific_transform = true;

        if(list)
        {
            if(list->size() == 7)
            {
                for(int i = 0; i < 3; i++) grasp_specific_translation[i] = list->get(i).asDouble();
                for(int i = 0; i < 4; i++) grasp_specific_orientation[i] = list->get(3 + i).asDouble();
            }
            else
            {
                yError() << prettyError(__FUNCTION__, "Invalid grasp_trsfm_right dimension in config. Should be 7.");
                valid_grasp_specific_transform = false;
            }
        }
        else valid_grasp_specific_transform = false;

        if( (!valid_grasp_specific_transform) && ((robot == "icubSim") || (robot == "icub")) )
        {
            yInfo() << "Loading grasp_trsfm_right default value for iCub";
            grasp_specific_translation[0] = -0.01;
            grasp_specific_orientation[1] = 1;
            grasp_specific_orientation[3] = - 38.0 * M_PI/180.0;
        }

        grasper_specific_transform_right = axis2dcm(grasp_specific_orientation);
        grasper_specific_transform_right.setSubcol(grasp_specific_translation, 0,3);
        yInfo() << "Grabber specific transform for right arm loaded\n" << grasper_specific_transform_right.toString();

        grasp_specific_translation.zero();
        grasp_specific_orientation.zero();
        list = rf.find("grasp_trsfm_left").asList();
        valid_grasp_specific_transform = true;

        if(list)
        {
            if(list->size() == 7)
            {
                for(int i = 0; i < 3; i++) grasp_specific_translation[i] = list->get(i).asDouble();
                for(int i = 0; i < 4; i++) grasp_specific_orientation[i] = list->get(3+i).asDouble();
            }
            else
            {
                yError() << prettyError(__FUNCTION__, "Invalid grasp_trsfm_left dimension in config. Should be 7.");
                valid_grasp_specific_transform = false;
            }
        }
        else valid_grasp_specific_transform = false;

        if( (!valid_grasp_specific_transform) && ((robot == "icubSim") || (robot == "icub")) )
        {
            yInfo() << "Loading grasp_trsfm_left default value for iCub";
            grasp_specific_translation[0] = -0.01;
            grasp_specific_orientation[1] = 1;
            grasp_specific_orientation[3] = + 38.0 * M_PI/180.0;
        }

        grasper_specific_transform_left = axis2dcm(grasp_specific_orientation);
        grasper_specific_transform_left.setSubcol(grasp_specific_translation, 0,3);
        yInfo() << "Grabber specific transform for left arm loaded\n" << grasper_specific_transform_left.toString();

        list = rf.find("approach_right").asList();
        grasper_approach_parameters_right.resize(4, 0.0);
        if (list)
        {
            if (list->size() == 4)
            {
                for(int i = 0; i < 4; i++) grasper_approach_parameters_right[i] = list->get(i).asDouble();
            }
            else
            {
                yError() << prettyError(__FUNCTION__, "Invalid approach_right dimension in config. Should be 4.");
            }
        }
        else if((robot == "icubSim") || (robot == "icub"))
        {
            grasper_approach_parameters_right[0] = -0.05;
            grasper_approach_parameters_right[1] = 0.0;
            grasper_approach_parameters_right[2] = 0.0;
            grasper_approach_parameters_right[3] = 0.0;
        }
        yInfo() << "Grabber specific approach for right arm loaded\n" << grasper_approach_parameters_right.toString();

        list = rf.find("approach_left").asList();
        grasper_approach_parameters_left.resize(4, 0.0);
        if (list)
        {
            if (list->size() == 4)
            {
                for(int i=0 ; i<4 ; i++) grasper_approach_parameters_left[i] = list->get(i).asDouble();
            }
            else
            {
                yError() << prettyError(__FUNCTION__, "Invalid approach_left dimension in config. Should be 4.");
            }
        }
        else if((robot == "icubSim") || (robot == "icub"))
        {
            grasper_approach_parameters_left[0] = -0.05;
            grasper_approach_parameters_left[1] = 0.0;
            grasper_approach_parameters_left[2] = +0.0;
            grasper_approach_parameters_left[3] = 0.0;
        }
        yInfo() << "Grabber specific approach for left arm loaded\n" << grasper_approach_parameters_left.toString();

        //  open the necessary ports
        point_cloud_rpc.open("/" + moduleName + "/pointCloud:rpc");
        action_render_rpc.open("/" + moduleName + "/actionRenderer:rpc");
        reach_calib_rpc.open("/" + moduleName + "/reachingCalibration:rpc");
        table_calib_rpc.open("/" + moduleName + "/tableCalib:rpc");
        user_rpc.open("/" + moduleName + "/cmd:rpc");
        sfm_rpc.open("/" + moduleName + "/sfm:rpc");
        img_in.open("/" + moduleName + "/img:i");


        //  open clients when using iCub

        if((robot == "icubSim") || (robot == "icub"))
        {
            Property optionLeftArm, optionRightArm;

            optionLeftArm.put("device", "cartesiancontrollerclient");
            optionLeftArm.put("remote", "/" + robot + "/cartesianController/left_arm");
            optionLeftArm.put("local", "/" + moduleName + "/cartesianClient/left_arm");

            optionRightArm.put("device", "cartesiancontrollerclient");
            optionRightArm.put("remote", "/" + robot + "/cartesianController/right_arm");
            optionRightArm.put("local", "/" + moduleName + "/cartesianClient/right_arm");

            if ((control_arms == "both") || (control_arms == "left"))
            {
                if (!left_arm_client.open(optionLeftArm))
                {
                    yError() << prettyError( __FUNCTION__, "Could not open cartesian solver client for left arm");
                    return false;
                }
            }
            if ((control_arms == "both") || (control_arms == "right"))
            {
                if (!right_arm_client.open(optionRightArm))
                {
                    if (left_arm_client.isValid())
                    {
                        left_arm_client.close();
                    }
                    yError() << prettyError( __FUNCTION__, "Could not open cartesian solver client for right arm");
                    return false;
                }
            }
        }

        //  attach callback
        attach(user_rpc);

        fixate_object = false;

        // PointCloud filtering
        radius = rf.check("radius_dbscan", Value(0.01)).asDouble();
        minpts = rf.check("points_dbscan", Value(10)).asInt();

        // Set Superquadric Model parameters
        double tol_superq = rf.check("tol_superq", Value(1e-5)).asDouble();
        int print_level_superq = rf.check("print_level_superq", Value(0)).asInt();
        object_class = rf.check("object_class", Value("default")).toString();
        int optimizer_points = rf.check("optimizer_points", Value(50)).asInt();
        bool random_sampling = rf.check("random_sampling", Value(true)).asBool();
        single_superq = rf.check("single_superq", Value(true)).asBool();

        estim.SetNumericValue("tol", tol_superq);
        estim.SetIntegerValue("print_level", print_level_superq);
        estim.SetStringValue("object_class", object_class);
        estim.SetIntegerValue("optimizer_points", optimizer_points);
        estim.SetBoolValue("random_sampling", random_sampling);

        bool merge_model = rf.check("merge_model", Value(true)).asBool();
        int minimum_points = rf.check("minimum_points", Value(150)).asInt();
        int fraction_pc = rf.check("fraction_pc", Value(8)).asInt();
        double threshold_axis = rf.check("tol_threshold_axissuperq", Value(0.7)).asDouble();
        double threshold_section1 = rf.check("threshold_section1", Value(0.6)).asDouble();
        double threshold_section2 = rf.check("threshold_section2", Value(0.03)).asDouble();

        estim.SetBoolValue("merge_model", merge_model);
        estim.SetIntegerValue("minimum_points", minimum_points);
        estim.SetIntegerValue("fraction_pc", fraction_pc);
        estim.SetNumericValue("threshold_axis", threshold_axis);
        estim.SetNumericValue("threshold_section1", threshold_section1);
        estim.SetNumericValue("threshold_section2", threshold_section2);

        // Set Superquadric Grasp parameters
        double tol_grasp = rf.check("tol_grasp", Value(1e-5)).asDouble();
        int print_level_grasp = rf.check("print_level_grasp", Value(0)).asInt();
        double constr_tol = rf.check("constr_tol", Value(1e-4)).asDouble();
        int max_superq = rf.check("max_superq", Value(4)).asInt();

        grasp_estim.SetNumericValue("tol", tol_grasp);
        grasp_estim.SetIntegerValue("print_level", print_level_grasp);
        grasp_estim.SetIntegerValue("max_superq", max_superq);
        grasp_estim.SetNumericValue("constr_tol", constr_tol);
        grasp_estim.SetStringValue("left_or_right", "right");

        list = rf.find("plane_table").asList();
        if (list)
        {
            if (list->size() == 4)
            {
                for(int i = 0 ; i < 4 ; i++) plane(i) = list->get(i).asDouble();
            }
        }
        else
        {
            plane << 0.0, 0.0, 1.0, 0.20;
        }

        grasp_estim.setVector("plane", plane);

        list = rf.find("displacement").asList();
        if (list)
        {
            if (list->size() == 3)
            {
                for(int i = 0 ; i < 3 ; i++) displacement(i) = list->get(i).asDouble();
            }
        }
        else
        {
            displacement << 0.0, 0.0, 0.0;
        }

        grasp_estim.setVector("displacement", displacement);


        hand.resize(11);
        list = rf.find("hand").asList();
        if (list)
        {
            if (list->size() == 11)
            {
                for(int i = 0 ; i < 11 ; i++) hand(i) = list->get(i).asDouble();
            }
        }
        else
        {
            hand << 0.03, 0.06, 0.03, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0;
        }

        grasp_estim.setVector("hand", hand);

        bounds_right.resize(6,2);
        list = rf.find("bounds_right").asList();
        if (list)
        {
            if (list->size() == 12)
            {
                for(int i = 0 ; i < 12 ; i = i+2)
                {
                  if ( i % 2 == 0 )
                      bounds_right(i/2,0) = list->get(i).asDouble();
                  else
                      bounds_right(i/2,1) = list->get(i).asDouble();
                }
            }
        }
        else
        {
            bounds_right << -0.5, 0.0, -0.2, 0.2, -0.3, 0.3, -M_PI, M_PI,-M_PI, M_PI,-M_PI, M_PI;
        }

        grasp_estim.setMatrix("bounds_right", bounds_right);

        bounds_left.resize(6,2);
        list = rf.find("bounds_left").asList();
        if (list)
        {
            if (list->size() == 12)
            {
                for(int i = 0 ; i < 12 ; i = i+2)
                {
                  if ( i % 2 == 0 )
                      bounds_left(i/2,0) = list->get(i).asDouble();
                  else
                      bounds_left(i/2,1) = list->get(i).asDouble();
                }
            }
        }
        else
        {
            bounds_left << -0.5, 0.0, -0.2, 0.2, -0.3, 0.3, -M_PI, M_PI,-M_PI, M_PI,-M_PI, M_PI;
        }

        grasp_estim.setMatrix("bounds_left", bounds_left);

        bounds_constr_right.resize(8,2);
        list = rf.find("bounds_constr_right").asList();
        if (list)
        {
            if (list->size() == 16)
            {
                for(int i = 0 ; i < 16 ; i++)
                {
                    if ( i % 2 == 0 )
                        bounds_constr_right(i/2,0) = list->get(i).asDouble();
                    else
                        bounds_constr_right(i/2,1) = list->get(i).asDouble();
                }
            }
        }
        else
        {
            bounds_constr_right << -10000, 0.0, -10000, 0.0, -10000, 0.0, 0.001,
                                     10.0, 0.0, 1.0, 0.00001, 10.0, 0.00001, 10.0, 0.00001, 10.0;
        }
        grasp_estim.setMatrix("bounds_constr_right", bounds_constr_right);

        bounds_constr_left.resize(8,2);
        list = rf.find("bounds_constr_left").asList();
        if (list)
        {
            if (list->size() == 16)
            {
                for(int i = 0 ; i < 16 ; i++)
                {
                    if ( i % 2 == 0 )
                        bounds_constr_left(i/2,0) = list->get(i).asDouble();
                    else
                        bounds_constr_left(i/2,1) = list->get(i).asDouble();
                }
            }
        }
        else
        {
            bounds_constr_left << -10000, 0.0, -10000, 0.0, -10000, 0.0, 0.01,
                                    10.0, 0.0, 1.0, 0.00001, 10.0, 0.00001, 10.0, 0.00001, 10.0;
        }
        grasp_estim.setMatrix("bounds_constr_left", bounds_constr_left);

        classes.push_back("box");
        classes.push_back("sphere");
        classes.push_back("cylinder");
        // Start visualization
        vis.visualize();

        return true;
    }


    /****************************************************************/
      bool updateModule() override
      {
          return false;
      }

      /****************************************************************/
      double getPeriod() override
      {
          return 1.0;
      }

      /****************************************************************/
      bool interruptModule() override
      {
          point_cloud_rpc.interrupt();
          action_render_rpc.interrupt();
          reach_calib_rpc.interrupt();
          user_rpc.interrupt();
          table_calib_rpc.interrupt();
          closing = true;

          return true;
      }

      /****************************************************************/
      bool close() override
      {
          point_cloud_rpc.close();
          action_render_rpc.close();
          reach_calib_rpc.close();
          user_rpc.close();
          table_calib_rpc.close();

          if (left_arm_client.isValid())
          {
              left_arm_client.close();
          }
          if (right_arm_client.isValid())
          {
              right_arm_client.close();
          }

          return true;
      }

      /****************************************************************/
      bool set_single_superq(const string &value)
      {
          single_superq = (value == "on");

          return true;
      }

      /****************************************************************/
      bool from_off_file(const string &object_file, const string &hand)
      {
          if (hand == "right")
          {
              grasping_hand = WhichHand::HAND_RIGHT;

              grasp_estim.SetStringValue("left_or_right", hand);
          }
          else if (hand == "left")
          {
              grasping_hand = WhichHand::HAND_LEFT;

              grasp_estim.SetStringValue("left_or_right", hand);
          }
          else if (hand == "both")
          {
              grasping_hand = WhichHand::BOTH;

              grasp_estim.SetStringValue("left_or_right", "right");
          }
          else
          {
              return false;
          }

          deque<Eigen::Vector3d> all_points;
          vector<Vector> points_yarp;
          vector<vector<unsigned char>> all_colors;

          ifstream fin(object_file);
          if (!fin.is_open())
          {
              yError() << "Unable to open file \"" << object_file;

              return 0;
          }

          Vector p(3);
          vector<unsigned int> c_(3);
          vector<unsigned char> c(3);

          string line;
          while (getline(fin,line))
          {
              istringstream iss(line);
              if (!(iss >> p(0) >> p(1) >> p(2)))
                  break;
              points_yarp.push_back(p);

              fill(c_.begin(),c_.end(),120);
              iss >> c_[0] >> c_[1] >> c_[2];
              c[0] = (unsigned char)c_[0];
              c[1] = (unsigned char)c_[1];
              c[2] = (unsigned char)c_[2];

              if (c[0] == c[1] && c[1] == c[2])
               {
                   c[0] = 50;
                   c[1] = 100;
                   c[2] = 0;
               }

              all_colors.push_back(c);
          }

          all_points = removeOutliers(points_yarp, all_colors);

          point_cloud.setPoints(all_points);
          point_cloud.setColors(all_colors);

          if (point_cloud.getNumberPoints() > 0)
          {
              computeSuperqAndGrasp(true);
              return true;
          }

          return false;
      }

      /****************************************************************/
      bool compute_superq_and_pose(const string &object_name, const string &hand)
      {
          if (hand == "right")
          {
              grasping_hand = WhichHand::HAND_RIGHT;

              grasp_estim.SetStringValue("left_or_right", hand);
          }
          else if (hand == "left")
          {
              grasping_hand = WhichHand::HAND_LEFT;

              grasp_estim.SetStringValue("left_or_right", hand);
          }
          else if (hand == "both")
          {
              grasping_hand = WhichHand::BOTH;

              grasp_estim.SetStringValue("left_or_right", "right");
          }
          else
          {
              return false;
          }

          fixate_object = true;

          bool ok;

          if (object_name != "hanging_tool")
             ok = requestPointCloud(object_name);
          else
             ok = acquireFromSFM();

          if (isInClasses(object_name))
              object_class = object_name;

          if (ok == true && point_cloud.getNumberPoints() > 0)
          {
              computeSuperqAndGrasp(true);
          }

          return ok;
      }

      /****************************************************************/
      bool grasp()
      {
          Vector best_pose;

          if (grasping_hand == WhichHand::BOTH)
          {
              if (best_hand == "right")
              {
                  Eigen::VectorXd pose;
                  pose.resize(7);
                  pose.head(3) = grasp_res_hand1.grasp_poses[grasp_res_hand1.best_pose].getGraspPosition();
                  pose.tail(4) = grasp_res_hand1.grasp_poses[grasp_res_hand1.best_pose].getGraspAxisAngle();
                  best_pose = eigenToYarp(pose);
              }
              else if (best_hand == "left")
              {
                  Eigen::VectorXd pose;
                  pose.resize(7);
                  pose.head(3) = grasp_res_hand2.grasp_poses[grasp_res_hand2.best_pose].getGraspPosition();
                  pose.tail(4) = grasp_res_hand2.grasp_poses[grasp_res_hand2.best_pose].getGraspAxisAngle();
                  best_pose = eigenToYarp(pose);
              }
          }
          else
          {
              Eigen::VectorXd pose;
              pose.resize(7);
              pose.head(3) = grasp_res_hand1.grasp_poses[grasp_res_hand1.best_pose].getGraspPosition();
              pose.tail(4) = grasp_res_hand1.grasp_poses[grasp_res_hand1.best_pose].getGraspAxisAngle();
              best_pose = eigenToYarp(pose);
          }

          executeGrasp(best_pose, best_hand);
      }

      /****************************************************************/
      bool drop()
      {
          if (action_render_rpc.getOutputCount() > 0)
          {
              Bottle cmd, reply;
              cmd.addVocab(Vocab::encode("drop"));
              action_render_rpc.write(cmd, reply);
              if (reply.get(0).asVocab() == Vocab::encode("ack"))
                  return true;
              else
                  return false;
          }
          else
          {
              return false;
          }
      }

      /****************************************************************/
      bool home()
      {
          if (action_render_rpc.getOutputCount() > 0)
          {
              Bottle cmd, reply;
              cmd.addVocab(Vocab::encode("home"));
              action_render_rpc.write(cmd, reply);
              if (reply.get(0).asVocab() == Vocab::encode("ack"))
                  return true;
              else
                  return false;

              return true;
          }
          else
          {
              return false;
          }
      }

      /************************************************************************/
      bool requestPointCloud(const string &object)
      {
         Bottle cmd_request;
         Bottle cmd_reply;

         //  if fixate_object is given, look at the object before acquiring the point cloud
         if (fixate_object)
         {
             if(action_render_rpc.getOutputCount() < 1)
             {
                 yError() << prettyError( __FUNCTION__,  "requestRefreshPointCloud: no connection to action rendering module");
                 return false;
             }

             cmd_request.addVocab(Vocab::encode("look"));
             cmd_request.addString(object);
             cmd_request.addString("wait");

             action_render_rpc.write(cmd_request, cmd_reply);
             if (cmd_reply.get(0).asVocab() != Vocab::encode("ack"))
             {
                 yError() << prettyError( __FUNCTION__,  "Didn't manage to look at the object");
                 return false;
             }
         }

         point_cloud.deletePoints();
         cmd_request.clear();
         cmd_reply.clear();

         yarp::sig::PointCloud<DataXYZRGBA> pc;

         cmd_request.addString("get_point_cloud");
         cmd_request.addString(object);

         if(point_cloud_rpc.getOutputCount() < 1)
         {
             yError() << prettyError( __FUNCTION__,  "requestRefreshPointCloud: no connection to point cloud module");
             return false;
         }

         point_cloud_rpc.write(cmd_request, cmd_reply);

         //  cheap workaround to get the point cloud
         Bottle* pcBt = cmd_reply.get(0).asList();
         bool success = pc.fromBottle(*pcBt);

         vector<Vector> acquired_points;
         vector<vector<unsigned char>> acquired_colors;

         Vector point(3);
         vector<unsigned char> c;
         c.resize(3);

         for (size_t i = 0; i < pc.size(); i++)
         {
            point(0)=pc(i).x; point(1)=pc(i).y; point(2)=pc(i).z;
            c[0] = pc(i).r;
            c[1] = pc(i).g;
            c[2] = pc(i).b;

            acquired_points.push_back(point);
            acquired_colors.push_back(c);
         }

         deque<Eigen::Vector3d> all_points;
         all_points = removeOutliers(acquired_points, acquired_colors);

         if (success && (pc.size() > 0))
         {
            point_cloud.setPoints(all_points);
            point_cloud.setColors(acquired_colors);
            return true;
         }
         else
            return false;
      }

      /****************************************************************/
      bool acquireFromSFM()
      {
          point_cloud.deletePoints();

          vector<Vector> acquired_points;
          vector<vector<unsigned char>> acquired_colors;

          Bottle cmd_request;
          Bottle cmd_reply;
          cmd_request.clear();
          cmd_reply.clear();
          cmd_request.addString("Points");
          Bottle pointsList;

          for (int i = u_i; i < u_f; i++)
          {
              for (int j = v_i; j < v_f; j++)
              {
                  Bottle &points = pointsList.addList();
                  cmd_request.addInt(i);
                  cmd_request.addInt(j);
                  points.addInt(i);
                  points.addInt(j);
              }
          }


          if (!sfm_rpc.write(cmd_request, cmd_reply))
          {
             yError() << " Problems in getting points from SFM ";
             return false;
          }

          ImageOf<PixelRgb> *inCamImg = img_in.read();

          for (int p_i = 0; p_i < cmd_reply.size()/3 ; p_i ++)
          {
              Vector point(3,0.0);

              point[0] = cmd_reply.get(p_i*3).asDouble();
              point[1] = cmd_reply.get(p_i*3 + 1).asDouble();
              point[2] = cmd_reply.get(p_i*3 + 2).asDouble();

              if (norm(point) == 0.0 || (point[0] > -0.2) || (point[0] < -0.6) || (point[2] < -0.2))
                  continue;

              Bottle *point2D = pointsList.get(p_i).asList();
              PixelRgb point_rgb = inCamImg->pixel(point2D->get(0).asInt(), point2D->get(1).asInt());

              vector<unsigned char> color(3);

              color[0] = point_rgb.r;
              color[1] = point_rgb.g;
              color[2] = point_rgb.b;

              acquired_points.push_back(point);
              acquired_colors.push_back(color);
          }

          filterPC(acquired_points, acquired_colors);

          deque<Eigen::Vector3d> eigen_points;

          for (size_t i = 0; i < acquired_points.size(); i++)
          {
              eigen_points.push_back(toEigen(acquired_points[i]));
          }

          point_cloud.setPoints(eigen_points);
          point_cloud.setColors(acquired_colors);

          if (point_cloud.getNumberPoints() > 0)
             return true;
          else
             return false;
      }

      /****************************************************************/
      void filterPC(vector<Vector> &point_cloud, vector<vector<unsigned char>> &colors)
      {
          double x_max = point_cloud[0][0];

          vector<Vector> new_point_cloud;
          vector<vector<unsigned char>> new_colors;

          for (size_t i = 1; i < point_cloud.size(); i++)
          {
              if (point_cloud[i][0] > x_max)
                 x_max = point_cloud[i][0];
          }

          for (size_t i = 0; i < point_cloud.size(); i++)
          {
              if (point_cloud[i][0] > x_max - 0.04)
              {
                 new_point_cloud.push_back(point_cloud[i]);

                 vector<unsigned char> color(3);
                 color[0] = colors[i][0];
                 color[1] = colors[i][1];
                 color[2] = colors[i][2];

                 new_colors.push_back(color);
              }
          }

          point_cloud.clear();
          colors.clear();

          for (size_t i = 0; i < new_point_cloud.size(); i++)
          {
              point_cloud.push_back(new_point_cloud[i]);
              colors.push_back(new_colors[i]);
          }
      }

      /****************************************************************/
      deque<Eigen::Vector3d> removeOutliers(vector<Vector> &points_yarp, vector<vector<unsigned char>> &all_colors)
      {
          double t0=Time::now();

          deque<Eigen::Vector3d> in_points;
          vector<vector<unsigned char>> in_colors;

          Property options;
          options.put("epsilon",radius);
          options.put("minpts",minpts);

          DBSCAN dbscan;
          map<size_t,set<size_t>> clusters=dbscan.cluster(points_yarp, options);

          size_t largest_class; size_t largest_size=0;
          for (auto it=begin(clusters); it!=end(clusters); it++)
          {
              if (it->second.size()>largest_size)
              {
                  largest_size=it->second.size();
                  largest_class=it->first;
              }
          }

          auto &c=clusters[largest_class];
          for (size_t i=0; i<points_yarp.size(); i++)
          {
              if (c.find(i)!=end(c))
              {
                  in_points.push_back(toEigen(points_yarp[i]));
                  in_colors.push_back(all_colors[i]);
              }
          }

          double t1=Time::now();

          cout << "|| ---------------------------------------------------- ||" << endl;
          cout<<"|| Outliers removed                                      : "<<points_yarp.size() - in_points.size()<<endl;
          cout << "|| ---------------------------------------------------- ||" << endl<<endl;

          all_colors = in_colors;

          return in_points;
       }

      /************************************************************************/
      void computeSuperqAndGrasp(bool choose_hand)
      {
          // Reset visualizer for new computations
          vis.resetSuperq();
          vis.resetPoses();
          vis.resetPoints();

          // Visualize acquired point cloud
          vis.addPoints(point_cloud, false);

          // Compute superq
          if (single_superq || object_class != "default")
          {
              estim.SetStringValue("object_class", object_class);
              superqs = estim.computeSuperq(point_cloud);
              vis.addPoints(point_cloud, true);
          }
          else
          {
              superqs = estim.computeMultipleSuperq(point_cloud);
              vis.addPoints(point_cloud, false);
          }
          // Visualize downsampled point cloud and estimated superq
          vis.addSuperq(superqs);

          // Get real value of table height
          getTable();

          // Compute grasp pose
          grasp_res_hand1 = grasp_estim.computeGraspPoses(superqs);

          // Show computed grasp pose and plane
          vis.addPoses(grasp_res_hand1.grasp_poses);
          vis.addPlane(grasp_estim.getPlaneHeight());

          // Compute and show grasp pose for the other hand
          if (grasping_hand == WhichHand::BOTH)
          {
              grasp_estim.SetStringValue("left_or_right", "left");
              grasp_res_hand2 = grasp_estim.computeGraspPoses(superqs);
              vis.addPoses(grasp_res_hand1.grasp_poses, grasp_res_hand2.grasp_poses);
          }

          if (choose_hand)
          {
              // TODO extend to R1 (see cardinal-grasp-pointss)
              if ((robot == "icubSim") || (robot == "icub"))
              {
                  if (grasping_hand == WhichHand::BOTH)
                  {
                      if (left_arm_client.isValid())
                      {
                          left_arm_client.view(icart_left);

                          computePoseHat(grasp_res_hand2, icart_left);
                      }

                      if (right_arm_client.isValid())
                      {
                          right_arm_client.view(icart_right);

                          computePoseHat(grasp_res_hand1, icart_right);
                      }
                  }
                  else if (grasping_hand == WhichHand::HAND_RIGHT)
                  {
                      if (right_arm_client.isValid())
                      {
                          right_arm_client.view(icart_right);

                          computePoseHat(grasp_res_hand1, icart_right);
                      }
                  }
                  else if (grasping_hand == WhichHand::HAND_LEFT)
                  {
                      if (left_arm_client.isValid())
                      {
                          left_arm_client.view(icart_left);

                          computePoseHat(grasp_res_hand1, icart_left);
                      }
                  }
              }
              else
              {
                  if(action_render_rpc.getOutputCount()<1)
                  {
                      yError() << prettyError( __FUNCTION__,  "getPoseCostFunction: no connection to action rendering module");
                  }
                  else
                  {
                      if (grasping_hand == WhichHand::BOTH)
                      {
                          computePoseHatR1(grasp_res_hand1, "right");
                          computePoseHatR1(grasp_res_hand2, "left");
                      }
                      else if (grasping_hand == WhichHand::HAND_RIGHT)
                      {
                          computePoseHatR1(grasp_res_hand1, "right");
                      }
                      else if (grasping_hand == WhichHand::HAND_LEFT)
                      {
                          computePoseHatR1(grasp_res_hand1, "left");
                      }
                  }
              }
              grasp_estim.refinePoseCost(grasp_res_hand1);

              if (grasping_hand == WhichHand::BOTH)
              {
                  grasp_estim.refinePoseCost(grasp_res_hand2);
                  vis.addPoses(grasp_res_hand1.grasp_poses, grasp_res_hand2.grasp_poses);
              }
              else
                  vis.addPoses(grasp_res_hand1.grasp_poses);

              if (grasping_hand == WhichHand::BOTH)
              {
                  int best_right = grasp_res_hand1.best_pose;
                  int best_left = grasp_res_hand2.best_pose;

                  if (grasp_res_hand1.grasp_poses[best_right].cost < grasp_res_hand2.grasp_poses[best_left].cost)
                  {
                      best_hand = "right";
                      vis.highlightBestPose("right", "both", best_right);
                  }
                  else
                  {
                      best_hand = "left";
                      vis.highlightBestPose("left", "both", best_left);
                  }
              }
              else if (grasping_hand == WhichHand::HAND_RIGHT)
              {
                  int best_right = grasp_res_hand1.best_pose;
                  best_hand = "right";
                  vis.highlightBestPose("right", "right", best_right);
              }
              else if (grasping_hand == WhichHand::HAND_LEFT)
              {
                  int best_left = grasp_res_hand1.best_pose;
                  best_hand = "left";
                  vis.highlightBestPose("left", "left", best_left);
              }
          }
      }

      /****************************************************************/
      void getTable()
      {
          bool table_ok = false;
          if (robot != "icubSim" && table_calib_rpc.getOutputCount() > 0)
          {
              Bottle table_cmd, table_rply;
              table_cmd.addVocab(Vocab::encode("get"));
              table_cmd.addString("table");

              table_calib_rpc.write(table_cmd, table_rply);
              if (Bottle *payload = table_rply.get(0).asList())
              {
                  if (payload->size() >= 2)
                  {
                      plane(0) = 0.0;
                      plane(1) = 0.0;
                      plane(2) = 1.0;

                      plane(3) = -payload->get(1).asDouble() + 0.035;
                      table_ok = true;

                      grasp_estim.setVector("plane", plane);
                  }
              }
          }
          if (!table_ok)
          {
            cout << "|| ---------------------------------------------------- ||" << endl;
            cout << "|| Unable to retrieve object table                      ||" << endl;
            cout << "|| Unsig default value                                  : " << -plane[3] << endl;
            cout << "|| ---------------------------------------------------- ||" << endl << endl;
          }
          else
          {
              cout << "|| ---------------------------------------------------- ||" << endl;
              cout << "|| Unsig plane value                                    : " << -plane[3] << endl;
              cout << "|| ---------------------------------------------------- ||" << endl << endl;
          }
      }

      /****************************************************************/
      void setGraspContext(ICartesianControl *icart)
      {
          //  set up the context for the grasping planning and execution
          //  enable all joints
          Vector dof;
          icart->getDOF(dof);
          Vector new_dof(10, 1);
          new_dof(1) = 0.0;
          icart->setDOF(new_dof, dof);
          icart->setPosePriority("position");
          icart->setInTargetTol(0.001);
        }

      /****************************************************************/
      Vector eigenToYarp(Eigen::VectorXd &v)
      {
          Vector x;
          x.resize(v.size());

          for (size_t i = 0; i< x.size(); i++)
          {
              x[i] = v[i];
          }

          return x;
      }

      /****************************************************************/
      void computePoseHat(GraspResults &grasp_res, ICartesianControl *icart)
      {
          for (size_t i = 0; i < grasp_res.grasp_poses.size(); i++)
          {
              int context_backup;
              icart->storeContext(&context_backup);

              //  set up the context for the computation of the candidates
              setGraspContext(icart);

              Eigen::VectorXd desired_pose = grasp_res.grasp_poses[i].getGraspPosition();
              Eigen::VectorXd desired_or = grasp_res.grasp_poses[i].getGraspAxisAngle();

              Vector x_d = eigenToYarp(desired_pose);
              Vector o_d = eigenToYarp(desired_or);

              Vector x_d_hat, o_d_hat, q_d_hat;

              bool success = icart->askForPose(x_d, o_d, x_d_hat, o_d_hat, q_d_hat);

              Eigen::VectorXd pose_hat = toEigen(x_d_hat);
              Eigen::VectorXd or_hat = toEigen(o_d_hat);

              Eigen::VectorXd robot_pose;
              robot_pose.resize(7);
              robot_pose.head(3) = pose_hat;
              robot_pose.tail(4) = or_hat;

              //for (size_t i = 0; i < grasp_res.grasp_poses.size(); i++)
              //{
              grasp_res.grasp_poses[i].setGraspParamsHat(robot_pose);
              //}

              //  restore previous context
              icart->restoreContext(context_backup);
              icart->deleteContext(context_backup);
          }
      }

      /****************************************************************/
      void computePoseHatR1(GraspResults &grasp_res, const string &hand)
      {
          for (size_t i = 0; i < grasp_res.grasp_poses.size(); i++)
          {
              Eigen::VectorXd desired_pose = grasp_res.grasp_poses[i].getGraspPosition();
              Eigen::VectorXd desired_or = grasp_res.grasp_poses[i].getGraspAxisAngle();

              Vector x_d = eigenToYarp(desired_pose);
              Vector o_d = eigenToYarp(desired_or);

              Bottle cmd, reply;
              cmd.addVocab(Vocab::encode("ask"));
              Bottle &subcmd = cmd.addList();
              for(int i=0 ; i<3 ; i++) subcmd.addDouble(x_d[i]);
              for(int i=0 ; i<4 ; i++) subcmd.addDouble(o_d[i]);

              cmd.addString(hand);

              action_render_rpc.write(cmd, reply);

              if(reply.size()<1)
              {
                  yError() << prettyError( __FUNCTION__,  "getPoseCostFunction: empty reply from action rendering module");
              }

              if(reply.get(0).asVocab() != Vocab::encode("ack"))
              {
                  yError() << prettyError( __FUNCTION__,  "getPoseCostFunction: invalid reply from action rendering module:") << reply.toString();
              }

              if(reply.size()<3)
              {
                  yError() << prettyError( __FUNCTION__,  "getPoseCostFunction: invlaid reply size from action rendering module") << reply.toString();
              }

              if(!reply.check("x"))
              {
                  yError() << prettyError( __FUNCTION__,  "getPoseCostFunction: invalid reply from action rendering module: missing x:") << reply.toString();
              }

              Bottle *position = reply.find("x").asList();
              Eigen::VectorXd robot_pose;
              robot_pose.resize(7);
              for(int i=0 ; i<3 ; i++) robot_pose(i) = position->get(i).asDouble();
              for(int i=3 ; i<7 ; i++) robot_pose(i) = position->get(i).asDouble();

              // for (size_t i = 0; i < grasp_res.grasp_poses.size(); i++)
              // {
              grasp_res.grasp_poses[i].setGraspParamsHat(robot_pose);
              // }
          }
      }

      /****************************************************************/
      bool take_tool()
      {
          Eigen::IOFormat CommaInitFmt(Eigen::StreamPrecision, Eigen::DontAlignCols,", ", ", ", "", "", " [ ", "]");

          Vector grasping_current_pos(3), grasping_current_o(4);
          icart_right->getPose(grasping_current_pos, grasping_current_o);

          grasping_current_pos[2] += 0.02;

          cout << "|| ---------------------------------------------------- ||"  << endl;
          cout << "|| Lifting a bit the tool                               :" << toEigen(grasping_current_pos).format(CommaInitFmt) << endl;

          icart_right->goToPoseSync(grasping_current_pos, grasping_current_o);
          icart_right->waitMotionDone();

          grasping_current_pos[0] += 0.08;

          cout << "|| ---------------------------------------------------- ||"  << endl;
          cout << "|| Moving the tool closer                               :" << toEigen(grasping_current_pos).format(CommaInitFmt) << endl;

          icart_right->goToPoseSync(grasping_current_pos, grasping_current_o);
          icart_right->waitMotionDone();

          if (best_hand == "right")
             grasping_current_pos[1] += 0.15;
          else
             grasping_current_pos[1] -= 0.15;

             cout << "|| ---------------------------------------------------- ||"  << endl;
             cout << "|| Moving the tool on the side                          :" << toEigen(grasping_current_pos).format(CommaInitFmt) << endl;

          icart_right->goToPoseSync(grasping_current_pos, grasping_current_o);
          icart_right->waitMotionDone();

          return true;
      }

      /****************************************************************/
      bool open_hand()
      {
          if (action_render_rpc.getOutputCount() > 0)
          {
              Bottle cmd, reply;
              cmd.addVocab(Vocab::encode("hand"));
              action_render_rpc.write(cmd, reply);
              if (reply.get(0).asVocab() == Vocab::encode("ack"))
                  return true;
              else
                  return false;
          }
          else
          {
              return false;
          }
      }

      /****************************************************************/
      bool executeGrasp(Vector &pose, string &best_hand)
      {
          Eigen::IOFormat CommaInitFmt(Eigen::StreamPrecision, Eigen::DontAlignCols,", ", ", ", "", "", " [ ", "]");

          if(robot == "icubSim")
          {
              //  simulation context, suppose there is no actionsRenderingEngine running
              if (best_hand == "right")
              {
                  Vector pose_intermediate(7,0.0);
                  pose_intermediate.setSubvector(0,pose.subVector(0,2) + grasper_approach_parameters_right.subVector(0,2));
                  pose_intermediate.setSubvector(3, pose.subVector(3,6));


                  int context_backup;
                  cout << "|| ---------------------------------------------------- ||"  << endl;
                  cout << "|| Right hand reaching  intermediate                    :" << toEigen(pose_intermediate).format(CommaInitFmt) << endl;

                  icart_right->storeContext(&context_backup);
                  setGraspContext(icart_right);

                  Vector previous_x(3), previous_o(4);
                  icart_right->getPose(previous_x, previous_o);

                  icart_right->goToPoseSync(pose_intermediate.subVector(0, 2), pose_intermediate.subVector(3,6));
                  icart_right->waitMotionDone();

                  cout << "|| Right hand reaching                                  :" << toEigen(pose).format(CommaInitFmt) << endl;
                  icart_right->goToPoseSync(pose.subVector(0, 2), pose.subVector(3,6));
                  icart_right->waitMotionDone();

                  // cout << "|| Right hand going back to                              :" << toEigen(previous_x).format(CommaInitFmt) << toEigen(previous_o).format(CommaInitFmt) << endl;
                  // cout << "|| ---------------------------------------------------- ||"  << endl;
                  // icart_right->goToPoseSync(previous_x, previous_o);
                  // icart_right->waitMotionDone();
                  //
                  // icart_right->restoreContext(context_backup);
                  // icart_right->deleteContext(context_backup);
                  return true;
              }
              else if (best_hand == "left")
              {

                  Vector pose_intermediate(7,0.0);
                  pose_intermediate.setSubvector(0,pose.subVector(0,2) + grasper_approach_parameters_left.subVector(0,2));
                  pose_intermediate.setSubvector(3, pose.subVector(3,6));

                  int context_backup;
                  cout << "|| ---------------------------------------------------- ||"  << endl;
                  cout << "|| Left hand reaching  pose intermediate                :" << toEigen(pose_intermediate).format(CommaInitFmt) << endl;

                  icart_left->storeContext(&context_backup);
                  setGraspContext(icart_left);

                  Vector previous_x(3), previous_o(4);
                  icart_left->getPose(previous_x, previous_o);

                  icart_left->goToPoseSync(pose_intermediate.subVector(0, 2), pose_intermediate.subVector(3,6));
                  icart_left->waitMotionDone();

                  cout << "|| ---------------------------------------------------- ||"  << endl;
                  cout << "|| Left hand reaching  pose                             :" << toEigen(pose).format(CommaInitFmt) << endl;
                  icart_left->goToPoseSync(pose.subVector(0, 2), pose.subVector(3,6));
                  icart_left->waitMotionDone();

                  // cout << "|| Left hand going back to                             :" << toEigen(previous_x).format(CommaInitFmt) << toEigen(previous_o).format(CommaInitFmt) << endl;
                  // cout << "|| ---------------------------------------------------- ||"  << endl;
                  // icart_left->goToPoseSync(previous_x, previous_o);
                  // icart_left->waitMotionDone();
                  // icart_left->restoreContext(context_backup);
                  // icart_left->deleteContext(context_backup);
                  return true;
              }
          }
          else
          {
              Vector old_pose = pose;

              cout<< "|| Pose to be fixed with calibration offsets              :" << toEigen(old_pose).format(CommaInitFmt)<< endl;
              fixReachingOffset(old_pose, pose);
              cout<< "|| Fixed pose                                             :" << toEigen(pose).format(CommaInitFmt)<< endl;
              //  communication with actionRenderingEngine/cmd:io
              //  grasp("cartesian" x y z gx gy gz theta) ("approach" (-0.05 0 +-0.05 0.0)) "left"/"right"
              Bottle command, reply;

              command.addString("grasp");
              Bottle &ptr = command.addList();
              ptr.addString("cartesian");
              ptr.addDouble(pose(0));
              ptr.addDouble(pose(1));
              ptr.addDouble(pose(2));
              ptr.addDouble(pose(3));
              ptr.addDouble(pose(4));
              ptr.addDouble(pose(5));
              ptr.addDouble(pose(6));


              Bottle &ptr1 = command.addList();
              ptr1.addString("approach");
              Bottle &ptr2 = ptr1.addList();
              if (grasping_hand == WhichHand::HAND_LEFT)
              {
                  for(int i=0 ; i<4 ; i++) ptr2.addDouble(grasper_approach_parameters_left[i]);
                  command.addString("left");
              }
              else
              {
                  for(int i=0 ; i<4 ; i++) ptr2.addDouble(grasper_approach_parameters_right[i]);
                  command.addString("right");
              }

              yInfo() << command.toString();
              action_render_rpc.write(command, reply);
              if (reply.toString() == "[ack]")
              {
                  return true;
              }
              else
              {
                  return false;
              }
          }

      }

      /****************************************************************/
      bool isInClasses(const string &obj_name)
      {
          auto it = find(classes.begin(), classes.end(), obj_name);
          if (it != classes.end())
          {
              cout << "|| ---------------------------------------------------- ||" << endl;
              cout<<"|| Object name is one of the classes                    ||"<<endl;
              cout << "|| ---------------------------------------------------- ||" << endl << endl;
              return true;
          }
          else
              return false;
      }

      /************************************************************************/
      bool attach(RpcServer &source)
      {
        return this->yarp().attachAsServer(source);
      }

      /****************************************************************/
      bool fixReachingOffset(const Vector &poseToFix, Vector &poseFixed,
                             const bool invert=false)
      {
          //  fix the pose offset accordint to iolReachingCalibration
          //  pose is supposed to be (x y z gx gy gz theta)
          if ((robot == "r1" || robot == "icub") && reach_calib_rpc.getOutputCount() > 0)
          {
              Bottle command, reply;

              command.addString("get_location_nolook");
              if (best_hand == "left")
              {
                  command.addString("iol-left");
              }
              else
              {
                  command.addString("iol-right");
              }

              command.addDouble(poseToFix(0));    //  x value
              command.addDouble(poseToFix(1));    //  y value
              command.addDouble(poseToFix(2));    //  z value
              command.addInt(invert?1:0);         //  flag to invert input/output map

              reach_calib_rpc.write(command, reply);

              //  incoming reply is going to be (success x y z)
              if (reply.size() < 2)
              {
                  yError() << prettyError( __FUNCTION__,  "Failure retrieving fixed pose");
                  return false;
              }
              else if (reply.get(0).asVocab() == Vocab::encode("ok"))
              {
                  poseFixed = poseToFix;
                  poseFixed(0) = reply.get(1).asDouble();
                  poseFixed(1) = reply.get(2).asDouble();
                  poseFixed(2) = reply.get(3).asDouble();
                  return true;
              }
              else
              {
                  yWarning() << "Couldn't retrieve fixed pose. Continuing with unchanged pose";
              }
          }
      }
};

int main(int argc, char *argv[])
{
    Network yarp;
    ResourceFinder rf;
    rf.setDefaultContext("Superquadric-Lib-Demo");
    rf.setDefaultConfigFile("config-icub.ini");
    rf.configure(argc, argv);

    if (!yarp.checkNetwork())
    {
        yError() << prettyError(__FUNCTION__, "YARP network not detected. Check nameserver");
        return EXIT_FAILURE;
    }

    SuperquadricPipelineDemo disp;

    return disp.runModule(rf);
}
