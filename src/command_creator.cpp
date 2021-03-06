#include "command_creator.h"

using namespace std;
geometry_msgs::Twist sharedTwist;

namespace rosdrone
{
// constructor
commandCreator::commandCreator(const ros::NodeHandle& ng, const ros::NodeHandle& np) : nh(ng), nhp(np)
{
  // initialize values
  getROSParameters();
  S <<  0, -1, 0, 1, 0, 0, 0, 0, 0;
  I = Eigen::Matrix3d::Identity();
  start_time = ros::Time::now().toSec();
  setRelativeBearingDesired();

  // initialize communications
  bearings_sub = nh.subscribe("/bearings", 2, &commandCreator::bearingMeasuresCallback, this);
  poseSub = nh.subscribe("/gazebo/model_states", 2, &commandCreator::posesCallback, this);
  formationControlSub = nh.subscribe("/formation_control", 2, &commandCreator::formationControlCallback, this);

  if(drone_ID == 2) errorFPub = nh.advertise<std_msgs::Float32>("errorF", 2);
  if(drone_ID == 1) errorDist = nh.advertise<std_msgs::Float32>("errorDist", 2);
  if(drone_ID == 1) desiredDist = nh.advertise<std_msgs::Float32>("desiredDist", 2);

  ROS_INFO("Command initialized");
}

// destructor
commandCreator::~commandCreator()
{
  // destructor contents
}

void commandCreator::spinCommand()
{
  calculateVelocityCommand();
  publishError();
  return;
}

void commandCreator::updateTwist()
{
  sharedTwist.linear.x = velocityCommand.u.x();
  sharedTwist.linear.y = velocityCommand.u.y();
  sharedTwist.linear.z = velocityCommand.u.z();

  sharedTwist.angular.x = 0;
  sharedTwist.angular.y = 0;
  sharedTwist.angular.z = velocityCommand.w;
}

void commandCreator::publishError()
{
  std_msgs::Float32 sum;
  sum.data = 0;
  for (auto& drone_measures : relativeBearing)
  {
    int i = drone_measures.first;
    for(auto& measure : drone_measures.second)
    {
      int j = measure.first;
      if(relativeBearingDesired[i].count(j))
        sum.data += (measure.second.bearing - relativeBearingDesired[i][j]).norm();
    }
  }
  if(drone_ID == 2) errorFPub.publish(sum);

  std_msgs::Float32 dist;
  dist.data = relativeBearing[1][2].distance;
  if(drone_ID == 1) errorDist.publish(dist);

  std_msgs::Float32 desDist;
  desDist.data = 2.0;
  if(drone_ID == 1) desiredDist.publish(desDist);
}

void commandCreator::setRelativeBearingDesired()
{
  std::vector<Eigen::Vector3d> drones;
  drones.emplace_back(-1, 0, -0.3);
  drones.emplace_back(sqrt(2)/2, sqrt(2)/2, 0.3);
  drones.emplace_back(sqrt(2)/2, -sqrt(2)/2, 0);

  Eigen::Matrix3d Rtemp;
  std::vector<Eigen::Matrix3d> R;
  Rtemp = Eigen::AngleAxisd(0.0, Eigen::Vector3d::UnitZ());
  R.push_back(Rtemp);
  Rtemp = Eigen::AngleAxisd(M_PI*7/8, Eigen::Vector3d::UnitZ());
  R.push_back(Rtemp);
  Rtemp = Eigen::AngleAxisd(-M_PI/2, Eigen::Vector3d::UnitZ());
  R.push_back(Rtemp);

  std::vector<std::pair<int, int> > pairs = { {1, 2}, {1,3}, {2,1}, {3,2} };

  for(auto pair : pairs)
    relativeBearingDesired[pair.first][pair.second] =
        (R[pair.first-1]*(drones[pair.second-1] - drones[pair.first-1])).normalized();
}

void commandCreator::calculateVelocityCommand()
{
  Eigen::Matrix3d Rij;

  auto my_measures = relativeBearing[drone_ID];

  int i = drone_ID;
  Eigen::Vector3d u = Eigen::Vector3d::Zero();
  double w = 0;

  for(auto measure : my_measures)
  {
    int j = measure.first;
    if(relativeBearingDesired[i].count(j))
    {
      Eigen::Vector3d bearing_ij = measure.second.bearing;
      u -= controlParams.kc * (I - bearing_ij*bearing_ij.transpose()) * relativeBearingDesired[i][j];
      w += controlParams.kc * bearing_ij.transpose() * S * relativeBearingDesired[i][j];
      if(i == 1 && j == 2) u += distanceController(measure.second.distance) * bearing_ij;
      if(i == 2 && j == 1) u += distanceController(measure.second.distance) * bearing_ij;
    }
  }

  for (auto drone_measures : relativeBearing)
  {
    if(drone_measures.first != i && drone_measures.second.count(i))
    {
      int j = drone_measures.first;
      if(relativeBearingDesired[j].count(i))
      {
        Eigen::Vector3d bearing_ji = drone_measures.second[i].bearing;
        Rij = Eigen::AngleAxisd(posesGazebo[j].psi - posesGazebo[i].psi, Eigen::Vector3d::UnitZ());
        u += controlParams.kc * Rij * (I - bearing_ji*bearing_ji.transpose()) * relativeBearingDesired[j][i];
      }
    }
  }

  nullSpaceMotions(u, w);

  velocityCommand = {u, w};
  updateTwist();
}

void commandCreator::nullSpaceMotions(Eigen::Vector3d& u, double& w)
{
  if(formation_control_active)
  {
    ROS_INFO_ONCE("Null-space motions initialized");
    Eigen::Vector3d centroid, translation_vel;
    double rotation, scale;
    for (auto drone : posesGazebo)
    {
      centroid += drone.second.p;
    }
    centroid /= posesGazebo.size();

    translation_vel = 0.3*(_position - centroid);

    rotation = _rotation;
    scale = _scale;

    auto poseInfo = posesGazebo[drone_ID];

    u += poseInfo.R.transpose() * (translation_vel + scale*(poseInfo.p - centroid) + rotation*S*(poseInfo.p - centroid));
    w += rotation;
  }
}

double commandCreator::distanceController(double distance)
{
  if (distController.last_time_measure == 0)
  {
    distController.last_time_measure = ros::Time::now().toSec();
    return 0;
  }

  double time = ros::Time::now().toSec();
  double dT = time - distController.last_time_measure;
  distController.last_time_measure = time;

  double e = distController.distDesired - distance;

  return - controlParams.kp_dist * e;
}

void commandCreator::getROSParameters()
{
  if (!nhp.getParam("uav_id", drone_ID))
  {
      ROS_ERROR("ROS parameter uav_id was not found!");
  }
}

double commandCreator::getYawFromQuaternion(const geometry_msgs::Quaternion& q)
{
  double siny_cosp = +2.0 * (q.w * q.z + q.x * q.y);
  double cosy_cosp = +1.0 - 2.0 * (q.y * q.y + q.z * q.z);
  return atan2(siny_cosp, cosy_cosp);
}

void commandCreator::bearingMeasuresCallback(const drones::Formation& measures)
{
  for (int i=0; i<measures.links.size(); i++)
  {
    string drone_name = measures.links[i].drone_name;
    // int drone_id = drone_name[drone_name.length()-1] - 48 - 3; // experimental
    int drone_id = drone_name[drone_name.length()-1] - 48; // simulation
    for (int j=0; j<measures.links[i].targets.size(); j++)
    {
      string target_name = measures.links[i].targets[j];
      // int target_id = target_name[target_name.length()-1] - 48 - 3; // experimental
      int target_id = target_name[target_name.length()-1] - 48; // simulation

      Eigen::Vector3d bearing;
      tf::vectorMsgToEigen(measures.links[i].bearings[j], bearing);

      relativeBearing[drone_id][target_id].bearing = bearing;
      relativeBearing[drone_id][target_id].distance = measures.links[i].distances[j].data;
    }
  }
}

void commandCreator::posesCallback(const gazebo_msgs::ModelStates& poses)
{
  for (int i = 0; i < poses.name.size(); i++)
  {
    if (poses.name[i].find("iris_") != std::string::npos)
    {
      int id = poses.name[i].at(poses.name[i].length() - 1) - 48;

      tf::pointMsgToEigen(poses.pose[i].position, posesGazebo[id].p);
      tf::quaternionMsgToEigen(poses.pose[i].orientation, posesGazebo[id].q);
      posesGazebo[id].q.normalize();
      posesGazebo[id].R = posesGazebo[id].q.toRotationMatrix();
      posesGazebo[id].psi = getYawFromQuaternion(poses.pose[i].orientation);
    }
  }
}

void commandCreator::formationControlCallback(const drones::FormationControl& control)
{
  formation_control_active = true;
  tf::vectorMsgToEigen(control.position, _position);
  _rotation = control.rotation.data;
  _scale = control.scale.data;
}
// end of namespace rosdrone_Command
}
