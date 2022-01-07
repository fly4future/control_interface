/* includes //{ */

#include <connection_result.h>
#include <eigen3/Eigen/Dense>
#include <eigen3/Eigen/src/Geometry/Quaternion.h>
#include <fog_msgs/msg/control_interface_diagnostics.hpp>
#include <fog_msgs/srv/path.hpp>
#include <fog_msgs/srv/path_to_local.hpp>
#include <fog_msgs/srv/get_px4_param_int.hpp>
#include <fog_msgs/srv/get_px4_param_float.hpp>
#include <fog_msgs/srv/set_px4_param_int.hpp>
#include <fog_msgs/srv/set_px4_param_float.hpp>
#include <fog_msgs/srv/vec4.hpp>
#include <fog_msgs/srv/waypoint_to_local.hpp>
#include <geometry_msgs/msg/pose_array.hpp>
#include <geometry_msgs/msg/transform_stamped.hpp>
#include <mavsdk/geometry.h>
#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/action/action.h>
#include <mavsdk/plugins/mission/mission.h>
#include <mavsdk/plugins/param/param.h>
#include <mutex>
#include <nav_msgs/msg/odometry.hpp>
#include <px4_msgs/msg/vehicle_control_mode.hpp>
#include <px4_msgs/msg/mission_result.hpp>
#include <px4_msgs/msg/home_position.hpp>
#include <px4_msgs/msg/vehicle_land_detected.hpp>
#include <rcl_interfaces/msg/set_parameters_result.hpp>
#include <rclcpp/qos.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/time.hpp>
#include <std_msgs/msg/color_rgba.hpp>
#include <std_srvs/srv/empty.hpp>
#include <std_srvs/srv/set_bool.hpp>
#include <std_srvs/srv/trigger.hpp>
#include <tf2/LinearMath/Matrix3x3.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>  // This has to be here otherwise you will get cryptic linker error about missing function 'getTimestamp'
#include <visualization_msgs/msg/marker_array.hpp>

//}

using namespace std::placeholders;

namespace control_interface
{

struct local_waypoint_t
{
  double x;
  double y;
  double z;
  double yaw;
};

struct gps_waypoint_t
{
  double latitude;
  double longitude;
  double altitude;
  double yaw;
};

/* getYaw //{ */
// TODO: Is this really what we want? Or do we want heading (angle from the x-axis in the XY plane) since that's what's published in the debugs?
double getYaw(const tf2::Quaternion &q) {
  double roll, pitch, yaw;
  tf2::Matrix3x3(q).getRPY(roll, pitch, yaw);
  return yaw;
}

double getYaw(const geometry_msgs::msg::Quaternion &q) {
  tf2::Quaternion tq;
  tq.setX(q.x);
  tq.setY(q.y);
  tq.setZ(q.z);
  tq.setW(q.w);
  double roll, pitch, yaw;
  tf2::Matrix3x3(tq).getRPY(roll, pitch, yaw);
  return yaw;
}
//}

/* angle conversions //{ */
double radToDeg(const double &angle_rad) {
  return angle_rad * 180.0 / M_PI;
}

double degToRad(const double &angle_deg) {
  return angle_deg * M_PI / 180.0;
}
//}

/* coordinate system conversions //{ */

/* globalToLocal //{ */
std::pair<double, double> globalToLocal(const std::shared_ptr<mavsdk::geometry::CoordinateTransformation> &coord_transform, const double &latitude_deg,
                                        const double &longitude_deg) {
  mavsdk::geometry::CoordinateTransformation::GlobalCoordinate global;
  global.latitude_deg  = latitude_deg;
  global.longitude_deg = longitude_deg;
  auto local           = coord_transform->local_from_global(global);

  return {local.east_m, local.north_m};
}

local_waypoint_t globalToLocal(const std::shared_ptr<mavsdk::geometry::CoordinateTransformation> &coord_transform, const gps_waypoint_t &wg) {
  local_waypoint_t                                             wl;
  mavsdk::geometry::CoordinateTransformation::GlobalCoordinate global;
  global.latitude_deg  = wg.latitude;
  global.longitude_deg = wg.longitude;
  auto local           = coord_transform->local_from_global(global);

  wl.x   = local.east_m;
  wl.y   = local.north_m;
  wl.z   = wg.altitude;
  wl.yaw = wg.yaw;
  return wl;
}

std::vector<local_waypoint_t> globalToLocal(const std::shared_ptr<mavsdk::geometry::CoordinateTransformation> &coord_transform,
                                            const std::vector<gps_waypoint_t> &                                wgs) {
  std::vector<local_waypoint_t> wls;
  for (const auto &wg : wgs) {
    wls.push_back(globalToLocal(coord_transform, wg));
  }
  return wls;
}
//}

/* localToGlobal //{ */
std::pair<double, double> localToGlobal(const std::shared_ptr<mavsdk::geometry::CoordinateTransformation> &coord_transform, const double &x, const double &y) {
  mavsdk::geometry::CoordinateTransformation::LocalCoordinate local;
  local.north_m = y;
  local.east_m  = x;
  auto global   = coord_transform->global_from_local(local);
  return {global.latitude_deg, global.longitude_deg};
}

gps_waypoint_t localToGlobal(const std::shared_ptr<mavsdk::geometry::CoordinateTransformation> &coord_transform, const local_waypoint_t &wl) {
  gps_waypoint_t                                              wg;
  mavsdk::geometry::CoordinateTransformation::LocalCoordinate local;
  local.north_m = wl.y;
  local.east_m  = wl.x;
  auto global   = coord_transform->global_from_local(local);
  wg.latitude   = global.latitude_deg;
  wg.longitude  = global.longitude_deg;
  wg.altitude   = wl.z;
  wg.yaw        = wl.yaw;
  return wg;
}

std::vector<gps_waypoint_t> localToGlobal(const std::shared_ptr<mavsdk::geometry::CoordinateTransformation> &coord_transform,
                                          const std::vector<local_waypoint_t> &                              wls) {
  std::vector<gps_waypoint_t> wgs;
  for (const auto &wl : wls) {
    wgs.push_back(localToGlobal(coord_transform, wl));
  }
  return wgs;
}
//}

//}

std::string to_string(const mavsdk::Mission::Result result);
std::string to_string(const mavsdk::Param::Result result);
std::string to_string(const mavsdk::ConnectionResult result);

// --------------------------------------------------------------
// |             ControlInterface class declaration             |
// --------------------------------------------------------------

/* class ControlInterface //{ */
class ControlInterface : public rclcpp::Node
{
public:
  ControlInterface(rclcpp::NodeOptions options);

private:
  std::mutex       state_mutex_;
  std::atomic_bool is_initialized_       = false; // set to true when MavSDK connection is established
  std::atomic_bool getting_landed_info_  = false; // set to true when a VehicleLandDetected is received from pixhawk
  std::atomic_bool getting_control_mode_ = false; // set to true when a VehicleControlMode is received from pixhawk

  std::mutex                       mission_mutex_;
  std::shared_ptr<mavsdk::Mission> mission_;
  bool mission_finished_flag_; // set to true in the missionResultCallback, this flag is cleared in the main controlRoutine
  enum mission_state_t
  {
    uploading,
    in_progress,
    finished
  } mission_state_ = finished;

  std::mutex mission_upload_mutex_;
  int mission_upload_attempts_;
  mavsdk::Mission::MissionPlan  mission_upload_waypoints_; // this buffer is used for repeated attempts at mission uploads
  enum mission_upload_state_t
  {
    started,
    done,
    failed
  } mission_upload_state_ = done;

  // the mavsdk::Action is used for requesting actions such as takeoff and landing
  std::mutex action_mutex_;
  std::shared_ptr<mavsdk::Action> action_;

  // the mavsdk::Param is used for setting and reading PX4 parameters
  std::mutex param_mutex_;
  std::shared_ptr<mavsdk::Param> param_;

  std::atomic_bool armed_                = false;
  std::atomic_bool takeoff_called_       = false;
  std::atomic_bool takeoff_completed_    = false;
  std::atomic_bool motion_started_       = false;
  std::atomic_bool landed_               = true;

  std::atomic_bool manual_override_     = false;

  std::atomic_bool gps_origin_set_ = false;
  std::atomic_bool getting_odom_   = false;

  unsigned         last_mission_instance_   = 1;
  rclcpp::Time     takeoff_time_;

  std::string uav_name_    = "";
  std::string world_frame_ = "";

  std::string                      device_url_;
  mavsdk::Mavsdk                   mavsdk_;
  std::shared_ptr<mavsdk::System>  system_;

  std::mutex                    waypoint_buffer_mutex_;  // guards the following block of variables
  std::vector<local_waypoint_t> waypoint_buffer_;        // this buffer is used for storing new incoming waypoints (it is moved to the mission_upload_waypoints_ buffer when upload starts)
  size_t                        last_mission_size_ = 0;

  Eigen::Vector4d desired_pose_;
  Eigen::Vector3d home_position_offset_ = Eigen::Vector3d(0, 0, 0);

  // use takeoff lat and long to initialize local frame
  std::mutex                                                  coord_transform_mutex_;
  std::shared_ptr<mavsdk::geometry::CoordinateTransformation> coord_transform_;

  // vehicle local position
  std::mutex pose_mutex_;
  std::vector<Eigen::Vector3d> pose_takeoff_samples_;
  Eigen::Vector3d              pose_pos_;
  tf2::Quaternion              pose_ori_;

  // config params
  double yaw_offset_correction_             = M_PI / 2;
  double takeoff_height_                    = 2.5;
  double takeoff_height_tolerance_          = 0.4;
  double takeoff_blocking_timeout_          = 3.0;
  double control_update_rate_               = 10.0;
  double waypoint_loiter_time_              = 0.0;
  bool   reset_octomap_before_takeoff_      = true;
  float  waypoint_acceptance_radius_        = 0.3f;
  float  altitude_acceptance_radius_        = 0.2f;
  double target_velocity_                   = 1.0;
  int    takeoff_position_samples_          = 20;
  int    mission_upload_attempts_threshold_ = 5;

  // publishers
  rclcpp::Publisher<geometry_msgs::msg::PoseStamped>::SharedPtr desired_pose_publisher_;  // https://ctu-mrs.github.io/docs/system/relative_commands.html
  rclcpp::Publisher<geometry_msgs::msg::PoseArray>::SharedPtr   waypoint_publisher_;
  rclcpp::Publisher<fog_msgs::msg::ControlInterfaceDiagnostics>::SharedPtr diagnostics_publisher_;

  // subscribers
  rclcpp::Subscription<px4_msgs::msg::VehicleControlMode>::SharedPtr  control_mode_subscriber_;
  rclcpp::Subscription<px4_msgs::msg::VehicleLandDetected>::SharedPtr land_detected_subscriber_;
  rclcpp::Subscription<px4_msgs::msg::MissionResult>::SharedPtr       mission_result_subscriber_;
  rclcpp::Subscription<px4_msgs::msg::HomePosition>::SharedPtr        home_position_subscriber_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr            odometry_subscriber_;

  OnSetParametersCallbackHandle::SharedPtr parameters_callback_handle_;

  // subscriber callbacks
  void controlModeCallback(const px4_msgs::msg::VehicleControlMode::UniquePtr msg);
  void landDetectedCallback(const px4_msgs::msg::VehicleLandDetected::UniquePtr msg);
  void missionResultCallback(const px4_msgs::msg::MissionResult::UniquePtr msg);
  void homePositionCallback(const px4_msgs::msg::HomePosition::UniquePtr msg);
  void odometryCallback(const nav_msgs::msg::Odometry::UniquePtr msg);

  // services provided
  rclcpp::Service<std_srvs::srv::SetBool>::SharedPtr          arming_service_;
  rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr          takeoff_service_;
  rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr          land_service_;
  rclcpp::Service<fog_msgs::srv::Vec4>::SharedPtr             local_waypoint_service_;
  rclcpp::Service<fog_msgs::srv::Path>::SharedPtr             local_path_service_;
  rclcpp::Service<fog_msgs::srv::Vec4>::SharedPtr             gps_waypoint_service_;
  rclcpp::Service<fog_msgs::srv::Path>::SharedPtr             gps_path_service_;
  rclcpp::Service<fog_msgs::srv::WaypointToLocal>::SharedPtr  waypoint_to_local_service_;
  rclcpp::Service<fog_msgs::srv::PathToLocal>::SharedPtr      path_to_local_service_;
  rclcpp::Service<fog_msgs::srv::SetPx4ParamInt>::SharedPtr   set_px4_param_int_service_;
  rclcpp::Service<fog_msgs::srv::GetPx4ParamInt>::SharedPtr   get_px4_param_int_service_;
  rclcpp::Service<fog_msgs::srv::SetPx4ParamFloat>::SharedPtr set_px4_param_float_service_;
  rclcpp::Service<fog_msgs::srv::GetPx4ParamFloat>::SharedPtr get_px4_param_float_service_;

  // service clients
  rclcpp::Client<std_srvs::srv::Empty>::SharedPtr octomap_reset_client_;

  // service callbacks
  bool armingCallback(const std::shared_ptr<std_srvs::srv::SetBool::Request> request, std::shared_ptr<std_srvs::srv::SetBool::Response> response);
  bool takeoffCallback(const std::shared_ptr<std_srvs::srv::Trigger::Request> request, std::shared_ptr<std_srvs::srv::Trigger::Response> response);
  bool landCallback(const std::shared_ptr<std_srvs::srv::Trigger::Request> request, std::shared_ptr<std_srvs::srv::Trigger::Response> response);
  bool localWaypointCallback(const std::shared_ptr<fog_msgs::srv::Vec4::Request> request, std::shared_ptr<fog_msgs::srv::Vec4::Response> response);
  bool localPathCallback(const std::shared_ptr<fog_msgs::srv::Path::Request> request, std::shared_ptr<fog_msgs::srv::Path::Response> response);
  bool gpsWaypointCallback(const std::shared_ptr<fog_msgs::srv::Vec4::Request> request, std::shared_ptr<fog_msgs::srv::Vec4::Response> response);
  bool gpsPathCallback(const std::shared_ptr<fog_msgs::srv::Path::Request> request, std::shared_ptr<fog_msgs::srv::Path::Response> response);
  bool waypointToLocalCallback(const std::shared_ptr<fog_msgs::srv::WaypointToLocal::Request> request,
                               std::shared_ptr<fog_msgs::srv::WaypointToLocal::Response>      response);
  bool pathToLocalCallback(const std::shared_ptr<fog_msgs::srv::PathToLocal::Request> request, std::shared_ptr<fog_msgs::srv::PathToLocal::Response> response);

  bool setPx4ParamIntCallback(const std::shared_ptr<fog_msgs::srv::SetPx4ParamInt::Request> request,
                              std::shared_ptr<fog_msgs::srv::SetPx4ParamInt::Response>      response);
  bool setPx4ParamFloatCallback(const std::shared_ptr<fog_msgs::srv::SetPx4ParamFloat::Request> request,
                                std::shared_ptr<fog_msgs::srv::SetPx4ParamFloat::Response>      response);
  bool getPx4ParamIntCallback(const std::shared_ptr<fog_msgs::srv::GetPx4ParamInt::Request> request,
                              std::shared_ptr<fog_msgs::srv::GetPx4ParamInt::Response>      response);
  bool getPx4ParamFloatCallback(const std::shared_ptr<fog_msgs::srv::GetPx4ParamFloat::Request> request,
                                std::shared_ptr<fog_msgs::srv::GetPx4ParamFloat::Response>      response);

  // parameter callback
  rcl_interfaces::msg::SetParametersResult parametersCallback(const std::vector<rclcpp::Parameter> &parameters);

  void publishDiagnostics();

  bool start_takeoff();
  bool land();
  bool startMission();
  bool startMissionUpload(const mavsdk::Mission::MissionPlan& mission_plan);
  bool stopMission();

  void publishDebugMarkers();
  void publishDesiredPose();

  // timers
  rclcpp::TimerBase::SharedPtr     control_timer_;
  rclcpp::TimerBase::SharedPtr     mavsdk_connection_timer_;
  void                             controlRoutine(void);
  void                             mavsdkConnectionRoutine(void);

  // helper state methods
  void state_mission_finished();
  void state_mission_uploading();
  void state_mission_in_progress();

  // utils
  template <class T>
  bool parse_param(const std::string &param_name, T &param_dest);

  bool canAddWaypoints(std::string& reason_out);
  template<typename T>
  bool addWaypoints(const T& path, const bool is_global, std::string& fail_reason_out);
  mavsdk::Mission::MissionItem to_mission_item(const local_waypoint_t& w_in);
  local_waypoint_t to_local_waypoint(const geometry_msgs::msg::PoseStamped& in, const bool is_global);
  local_waypoint_t to_local_waypoint(const fog_msgs::srv::WaypointToLocal::Request& in, const bool is_global);
  local_waypoint_t to_local_waypoint(const std::vector<double>& in, const bool is_global);
  local_waypoint_t to_local_waypoint(const Eigen::Vector4d& in, const bool is_global);

  rclcpp::CallbackGroup::SharedPtr new_cbk_grp();
};
//}

// --------------------------------------------------------------
// |            ControlInterface class implementation           |
// --------------------------------------------------------------

/* constructor //{ */
ControlInterface::ControlInterface(rclcpp::NodeOptions options) : Node("control_interface", options) {

  RCLCPP_INFO(get_logger(), "[%s]: Initializing...", get_name());

  try
  {
    uav_name_ = std::string(std::getenv("DRONE_DEVICE_ID"));
  }
  catch (...)
  {
    RCLCPP_ERROR(get_logger(), "[%s]: Environment variable DRONE_DEVICE_ID was not defined!", get_name());
  }
  RCLCPP_INFO(get_logger(), "[%s]: UAV name is: '%s'", get_name(), uav_name_.c_str());


  RCLCPP_INFO(get_logger(), "-------------- Loading parameters --------------");

  /* parse params from launch file //{ */
  bool loaded_successfully = true;
  loaded_successfully &= parse_param("device_url", device_url_);
  loaded_successfully &= parse_param("world_frame", world_frame_);
  //}

  /* parse params from config file //{ */
  loaded_successfully &= parse_param("general.reset_octomap_before_takeoff", reset_octomap_before_takeoff_);
  loaded_successfully &= parse_param("general.control_update_rate", control_update_rate_);

  loaded_successfully &= parse_param("takeoff.height", takeoff_height_);
  loaded_successfully &= parse_param("takeoff.height_tolerance", takeoff_height_tolerance_);
  loaded_successfully &= parse_param("takeoff.blocking_timeout", takeoff_blocking_timeout_);
  loaded_successfully &= parse_param("takeoff.position_samples", takeoff_position_samples_);

  loaded_successfully &= parse_param("px4.target_velocity", target_velocity_);
  loaded_successfully &= parse_param("px4.waypoint_loiter_time", waypoint_loiter_time_);
  loaded_successfully &= parse_param("px4.waypoint_acceptance_radius", waypoint_acceptance_radius_);
  loaded_successfully &= parse_param("px4.altitude_acceptance_radius", altitude_acceptance_radius_);

  loaded_successfully &= parse_param("mavsdk.yaw_offset_correction", yaw_offset_correction_);
  loaded_successfully &= parse_param("mavsdk.mission_upload_attempts_threshold", mission_upload_attempts_threshold_);

  if (!loaded_successfully) {
    RCLCPP_ERROR_STREAM(get_logger(), "Could not load all non-optional parameters. Shutting down.");
    rclcpp::shutdown();
    return;
  }

  if (control_update_rate_ < 5.0) {
    control_update_rate_ = 5.0;
    RCLCPP_WARN(get_logger(), "[%s]: Control update rate set too slow. Defaulting to 5 Hz", get_name());
  }

  //}

  // | ------------- misc. parameters initialization ------------ |
  desired_pose_ = Eigen::Vector4d(0.0, 0.0, 0.0, 0.0);

  /* estabilish connection with PX4 //{ */
  mavsdk::ConnectionResult connection_result;
  try
  {
    // Matouš: this shouldn't throw according to the documentation - why is it in a try/catch?
    connection_result = mavsdk_.add_any_connection(device_url_);
  }
  catch (...)
  {
    RCLCPP_ERROR(get_logger(), "[%s]: Connection failed! Device does not exist: %s", get_name(), device_url_.c_str());
    exit(EXIT_FAILURE);
  }
  if (connection_result != mavsdk::ConnectionResult::Success)
  {
    RCLCPP_ERROR(get_logger(), "[%s]: Connection failed: %s", get_name(), to_string(connection_result).c_str());
    exit(EXIT_FAILURE);
  }
  else
  {
    RCLCPP_INFO(get_logger(), "[%s]: MAVSDK connected to device: %s", get_name(), device_url_.c_str());
  }

  //}

  rclcpp::QoS qos(rclcpp::KeepLast(3));
  // | ------------------ initialize publishers ----------------- |
  desired_pose_publisher_ = create_publisher<geometry_msgs::msg::PoseStamped>("~/desired_pose_out", qos);
  waypoint_publisher_     = create_publisher<geometry_msgs::msg::PoseArray>("~/waypoints_out", qos);
  diagnostics_publisher_  = create_publisher<fog_msgs::msg::ControlInterfaceDiagnostics>("~/diagnostics_out", qos);

  // service clients
  octomap_reset_client_ = create_client<std_srvs::srv::Empty>("~/octomap_reset_out");

  // | ------------------ initialize callbacks ------------------ |

  parameters_callback_handle_ = add_on_set_parameters_callback(std::bind(&ControlInterface::parametersCallback, this, _1));

  rclcpp::SubscriptionOptions subopts;

  // create a mutually exclusive callback group for each callback so that only a single instance of each callback can be running at one time
  subopts.callback_group = new_cbk_grp();
  control_mode_subscriber_ = create_subscription<px4_msgs::msg::VehicleControlMode>("~/control_mode_in",
      rclcpp::SystemDefaultsQoS(), std::bind(&ControlInterface::controlModeCallback, this, _1), subopts);

  subopts.callback_group = new_cbk_grp();
  land_detected_subscriber_ = create_subscription<px4_msgs::msg::VehicleLandDetected>("~/land_detected_in",
      rclcpp::SystemDefaultsQoS(), std::bind(&ControlInterface::landDetectedCallback, this, _1), subopts);

  subopts.callback_group = new_cbk_grp();
  mission_result_subscriber_ = create_subscription<px4_msgs::msg::MissionResult>("~/mission_result_in",
      rclcpp::SystemDefaultsQoS(), std::bind(&ControlInterface::missionResultCallback, this, _1), subopts);

  subopts.callback_group = new_cbk_grp();
  home_position_subscriber_ = create_subscription<px4_msgs::msg::HomePosition>("~/home_position_in",
      rclcpp::SystemDefaultsQoS(), std::bind(&ControlInterface::homePositionCallback, this, _1), subopts);

  subopts.callback_group = new_cbk_grp();
  odometry_subscriber_ = create_subscription<nav_msgs::msg::Odometry>("~/local_odom_in",
      rclcpp::SystemDefaultsQoS(), std::bind(&ControlInterface::odometryCallback, this, _1), subopts);

  // service handlers
  const auto qos_profile = qos.get_rmw_qos_profile();
  const auto action_grp_ptr = new_cbk_grp();
  arming_service_  = create_service<std_srvs::srv::SetBool>("~/arming_in",
      std::bind(&ControlInterface::armingCallback, this, _1, _2), qos_profile, action_grp_ptr);

  takeoff_service_ = create_service<std_srvs::srv::Trigger>("~/takeoff_in",
      std::bind(&ControlInterface::takeoffCallback, this, _1, _2), qos_profile, action_grp_ptr);

  land_service_ = create_service<std_srvs::srv::Trigger>("~/land_in",
      std::bind(&ControlInterface::landCallback, this, _1, _2), qos_profile, action_grp_ptr);

  const auto waypt_grp_ptr = new_cbk_grp();
  local_waypoint_service_ = create_service<fog_msgs::srv::Vec4>("~/local_waypoint_in",
      std::bind(&ControlInterface::localWaypointCallback, this, _1, _2), qos_profile, waypt_grp_ptr);

  local_path_service_ = create_service<fog_msgs::srv::Path>("~/local_path_in",
      std::bind(&ControlInterface::localPathCallback, this, _1, _2), qos_profile, waypt_grp_ptr);

  gps_waypoint_service_ = create_service<fog_msgs::srv::Vec4>("~/gps_waypoint_in",
      std::bind(&ControlInterface::gpsWaypointCallback, this, _1, _2), qos_profile, waypt_grp_ptr);

  gps_path_service_ = create_service<fog_msgs::srv::Path>("~/gps_path_in",
      std::bind(&ControlInterface::gpsPathCallback, this, _1, _2), qos_profile, waypt_grp_ptr);

  waypoint_to_local_service_ = create_service<fog_msgs::srv::WaypointToLocal>("~/waypoint_to_local_in",
      std::bind(&ControlInterface::waypointToLocalCallback, this, _1, _2), qos_profile, waypt_grp_ptr);

  path_to_local_service_ = create_service<fog_msgs::srv::PathToLocal>("~/path_to_local_in",
      std::bind(&ControlInterface::pathToLocalCallback, this, _1, _2), qos_profile, waypt_grp_ptr);

  const auto param_grp_ptr = new_cbk_grp();
  set_px4_param_int_service_ = create_service<fog_msgs::srv::SetPx4ParamInt>("~/set_px4_param_int_in",
      std::bind(&ControlInterface::setPx4ParamIntCallback, this, _1, _2), qos_profile, param_grp_ptr);
  get_px4_param_int_service_ = create_service<fog_msgs::srv::GetPx4ParamInt>("~/get_px4_param_int_in",
      std::bind(&ControlInterface::getPx4ParamIntCallback, this, _1, _2), qos_profile, param_grp_ptr);
  set_px4_param_float_service_ = create_service<fog_msgs::srv::SetPx4ParamFloat>("~/set_px4_param_float_in",
      std::bind(&ControlInterface::setPx4ParamFloatCallback, this, _1, _2), qos_profile, param_grp_ptr);
  get_px4_param_float_service_ = create_service<fog_msgs::srv::GetPx4ParamFloat>("~/get_px4_param_float_in",
      std::bind(&ControlInterface::getPx4ParamFloatCallback, this, _1, _2), qos_profile, param_grp_ptr);

  control_timer_ = create_wall_timer(std::chrono::duration<double>(1.0 / control_update_rate_),
      std::bind(&ControlInterface::controlRoutine, this), new_cbk_grp());

  mavsdk_connection_timer_ = create_wall_timer(std::chrono::duration<double>(1.0),
      std::bind(&ControlInterface::mavsdkConnectionRoutine, this), new_cbk_grp());
}
//}

/* parametersCallback //{ */
rcl_interfaces::msg::SetParametersResult ControlInterface::parametersCallback(const std::vector<rclcpp::Parameter> &parameters) {
  rcl_interfaces::msg::SetParametersResult result;
  result.successful = false;
  result.reason     = "";

  for (const auto &param : parameters)
  {
    std::stringstream result_ss;

    /* takeoff_height //{ */
    if (param.get_name() == "takeoff.height") {
      if (param.get_type() == rclcpp::ParameterType::PARAMETER_DOUBLE) {
        if (param.as_double() >= 0.5 && param.as_double() < 10) {
          takeoff_height_   = param.as_double();
          result.successful = true;
          RCLCPP_INFO(get_logger(), "[%s]: Parameter: '%s' set to %1.2f", get_name(), param.get_name().c_str(), param.as_double());
        } else {
          result_ss << "parameter '" << param.get_name() << "' cannot be set to " << param.as_double() << " because it is not in range <0.5;10>";
          result.reason = result_ss.str();
        }
      } else {
        result_ss << "parameter '" << param.get_name() << "' has to be type DOUBLE";
        result.reason = result_ss.str();
      }
      //}

      /* waypoint_loiter_time //{ */
    } else if (param.get_name() == "px4.waypoint_loiter_time") {
      if (param.get_type() == rclcpp::ParameterType::PARAMETER_DOUBLE) {
        if (param.as_double() >= 0.0) {
          waypoint_loiter_time_ = param.as_double();
          result.successful     = true;
          RCLCPP_INFO(get_logger(), "[%s]: Parameter: '%s' set to %1.2f", get_name(), param.get_name().c_str(), param.as_double());
        } else {
          result_ss << "parameter '" << param.get_name() << "' cannot be set to " << param.as_double() << " because it is a negative value";
          result.reason = result_ss.str();
        }
      } else {
        result_ss << "parameter '" << param.get_name() << "' has to be type DOUBLE";
        result.reason = result_ss.str();
      }
      //}

      /* reset_octomap_before_takeoff //{ */
    } else if (param.get_name() == "general.reset_octomap_before_takeoff") {
      if (param.get_type() == rclcpp::ParameterType::PARAMETER_BOOL) {
        reset_octomap_before_takeoff_ = param.as_bool();
        result.successful             = true;
        RCLCPP_INFO(get_logger(), "[%s]: Parameter: '%s' set to %s", get_name(), param.get_name().c_str(), param.as_bool() ? "TRUE" : "FALSE");
      } else {
        result_ss << "parameter '" << param.get_name() << "' has to be type BOOL";
        result.reason = result_ss.str();
      }
      //}

    } else {
      result_ss << "parameter '" << param.get_name() << "' cannot be changed dynamically";
      result.reason = result_ss.str();
    }
  }

  if (!result.successful) {
    RCLCPP_WARN(get_logger(), "[%s]: Failed to set parameter: %s", get_name(), result.reason.c_str());
  }

  return result;
}
//}

/* controlModeCallback //{ */
void ControlInterface::controlModeCallback(const px4_msgs::msg::VehicleControlMode::UniquePtr msg)
{
  std::scoped_lock lck(state_mutex_, mission_mutex_, mission_upload_mutex_, waypoint_buffer_mutex_);

  if (!is_initialized_)
    return;

  getting_control_mode_ = true;

  static bool previous_manual_flag = true;
  const bool current_manual_flag  = msg->flag_control_manual_enabled;

  if (!previous_manual_flag && current_manual_flag)
  {
    RCLCPP_INFO(get_logger(), "[%s]: Control flag switched to manual. Stopping and clearing mission", get_name());
    manual_override_.store(true);
    if (!stopMission())
      RCLCPP_ERROR(get_logger(), "[%s]: Previous mission cannot be stopped. Manual landing required", get_name());
  }

  previous_manual_flag = msg->flag_control_manual_enabled;

  if (armed_ != msg->flag_armed)
  {
    armed_ = msg->flag_armed;
    if (armed_)
    {
      RCLCPP_WARN(get_logger(), "[%s]: Vehicle armed", get_name());
    }
    else
    {
      RCLCPP_WARN(get_logger(), "[%s]: Vehicle disarmed", get_name());
      if (landed_ && manual_override_)
      {
        RCLCPP_INFO(get_logger(), "[%s]: Vehicle landed, auto control will be re-enabled", get_name());
        manual_override_ = false;
      }
    }
  }
}
//}

/* landDetectedCallback //{ */
void ControlInterface::landDetectedCallback(const px4_msgs::msg::VehicleLandDetected::UniquePtr msg)
{
  std::scoped_lock lck(state_mutex_);

  if (!is_initialized_)
    return;

  getting_landed_info_ = true;
  // checking only ground_contact flag instead of landed due to a problem in simulation
  landed_ = msg->ground_contact;
}
//}

/* missionResultCallback //{ */
void ControlInterface::missionResultCallback(const px4_msgs::msg::MissionResult::UniquePtr msg)
{
  std::scoped_lock lck(mission_mutex_, state_mutex_);

  if (!is_initialized_)
    return;

  // check if a mission is currently in-progress and we got an indication that it is finished
  if (mission_state_ == mission_state_t::in_progress && msg->finished && msg->instance_count != last_mission_instance_)
  {
    RCLCPP_INFO(get_logger(), "[%s]: Mission #%u finished by callback", get_name(), msg->instance_count);
    mission_finished_flag_ = true; // set the appropriate flag to signal the state machine to change state
    last_mission_instance_ = msg->instance_count;
  }
}
//}

/* homePositionCallback //{ */
void ControlInterface::homePositionCallback(const px4_msgs::msg::HomePosition::UniquePtr msg)
{
  std::scoped_lock lck(state_mutex_, coord_transform_mutex_);

  if (!is_initialized_)
    return;

  mavsdk::geometry::CoordinateTransformation::GlobalCoordinate ref;
  ref.latitude_deg  = msg->lat;
  ref.longitude_deg = msg->lon;

  coord_transform_ = std::make_shared<mavsdk::geometry::CoordinateTransformation>(mavsdk::geometry::CoordinateTransformation(ref));

  RCLCPP_INFO(get_logger(), "[%s]: GPS origin set! Lat: %.6f, Lon: %.6f", get_name(), ref.latitude_deg, ref.longitude_deg);

  home_position_offset_ = Eigen::Vector3d(msg->y, msg->x, -msg->z);
  RCLCPP_INFO(get_logger(), "[%s]: Home position offset (local): %.2f, %.2f, %.2f", get_name(), home_position_offset_.x(),
              home_position_offset_.y(), home_position_offset_.z());

  gps_origin_set_ = true;
}
//}

/* odometryCallback //{ */
void ControlInterface::odometryCallback(const nav_msgs::msg::Odometry::UniquePtr msg)
{
  std::scoped_lock lck(state_mutex_, pose_mutex_);

  if (!is_initialized_)
    return;

  getting_odom_ = true;
  RCLCPP_INFO_ONCE(get_logger(), "[%s]: Getting odometry", get_name());

  pose_pos_.x() = msg->pose.pose.position.x;
  pose_pos_.y() = msg->pose.pose.position.y;
  pose_pos_.z() = msg->pose.pose.position.z;
  pose_ori_.setX(msg->pose.pose.orientation.x);
  pose_ori_.setY(msg->pose.pose.orientation.y);
  pose_ori_.setZ(msg->pose.pose.orientation.z);
  pose_ori_.setW(msg->pose.pose.orientation.w);

  const Eigen::Vector3d pos(msg->pose.pose.position.x, msg->pose.pose.position.y, msg->pose.pose.position.z);
  pose_takeoff_samples_.push_back(pos);
  if (pose_takeoff_samples_.size() > (size_t)takeoff_position_samples_)
    pose_takeoff_samples_.erase(pose_takeoff_samples_.begin());

  if (takeoff_called_ && !manual_override_)
  {
    const double time_since_takeoff = get_clock()->now().seconds() - takeoff_time_.seconds();
    if (std::abs(msg->pose.pose.position.z - desired_pose_.z()) < takeoff_height_tolerance_ || time_since_takeoff > takeoff_blocking_timeout_)
    {
      RCLCPP_INFO(get_logger(), "[%s]: Takeoff completed", get_name());
      takeoff_completed_ = true;
      takeoff_called_ = false;
    }
  }
}
//}

/* takeoffCallback //{ */
bool ControlInterface::takeoffCallback([[maybe_unused]] const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
                                       std::shared_ptr<std_srvs::srv::Trigger::Response>                       response)
{
  std::scoped_lock lck(state_mutex_, action_mutex_, pose_mutex_, waypoint_buffer_mutex_);

  if (!is_initialized_)
  {
    response->success = false;
    response->message = "Takeoff rejected, not initialized";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  if (!gps_origin_set_)
  {
    response->success = false;
    response->message = "Takeoff rejected, GPS origin not set";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  if (!armed_)
  {
    response->success = false;
    response->message = "Takeoff rejected, vehicle not armed";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  if (!landed_)
  {
    response->success = false;
    response->message = "Takeoff rejected, vehicle not landed";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  const bool success = start_takeoff();
  if (success)
  {
    response->success = true;
    response->message = "Taking off";
    return true;
  }

  response->success = false;
  response->message = "Takeoff rejected";
  return true;
}
//}

/* landCallback //{ */
bool ControlInterface::landCallback([[maybe_unused]] const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
                                    std::shared_ptr<std_srvs::srv::Trigger::Response>                       response)
{
  std::scoped_lock lck(state_mutex_, mission_mutex_, waypoint_buffer_mutex_, action_mutex_);

  if (!is_initialized_)
  {
    response->success = false;
    response->message = "Landing rejected, not initialized";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  if (!armed_)
  {
    response->success = false;
    response->message = "Landing rejected, vehicle not armed";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  if (landed_)
  {
    response->success = false;
    response->message = "Landing rejected, vehicle not airborne";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  const bool success = stopMission() && land();
  if (success)
  {
    response->success = true;
    response->message = "Landing";
    return true;
  }
  response->success = false;
  response->message = "Landing rejected";
  return true;
}
//}

/* armingCallback //{ */
bool ControlInterface::armingCallback([[maybe_unused]] const std::shared_ptr<std_srvs::srv::SetBool::Request> request,
                                      std::shared_ptr<std_srvs::srv::SetBool::Response>                       response)
{
  std::scoped_lock lck(state_mutex_, action_mutex_);

  if (!is_initialized_)
  {
    response->success = false;
    response->message = "Arming rejected, not initialized";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  if (request->data)
  {
    const auto result = action_->arm();
    if (result != mavsdk::Action::Result::Success)
    {
      response->message = "Arming failed";
      response->success = false;
      RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
      return true;
    }
    else
    {
      response->message = "Vehicle armed";
      response->success = true;
      armed_ = true;
      RCLCPP_WARN(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
      return true;
    }
  }
  else
  {
    const auto result = action_->disarm();
    if (result != mavsdk::Action::Result::Success)
    {
      response->message = "Disarming failed";
      response->success = false;
      RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
      return true;
    }
    else
    {
      response->message = "Vehicle disarmed";
      response->success = true;
      armed_ = false;
      takeoff_completed_.store(false);
      RCLCPP_WARN(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
      return true;
    }
  }
}
//}

/* canAddWaypoints() method //{ */
// helper method that performs the necessary checks before adding a waypoint to the waypoint_buffer_
bool ControlInterface::canAddWaypoints(std::string& reason_out)
{
  if (!is_initialized_)
  {
    reason_out = "not initialized";
    return false;
  }

  if (!gps_origin_set_)
  {
    reason_out = "missing GPS origin";
    return false;
  }

  if (landed_)
  {
    reason_out = "vehicle not airborne";
    return false;
  }

  if (manual_override_)
  {
    reason_out = "vehicle is under manual control";
    return false;
  }

  if (!takeoff_completed_)
  {
    reason_out = "vehicle not flying normally";
    return false;
  }

  return true;
}
//}

/* addWaypoints() method //{ */
template<typename T>
bool ControlInterface::addWaypoints(const T& path, const bool is_global, std::string& fail_reason_out)
{
  // check that we are in a state in which we can add waypoints to the buffer
  if (!canAddWaypoints(fail_reason_out))
    return true;

  // stop the current mission (if any)
  if (!stopMission())
  {
    fail_reason_out = "previous mission cannot be aborted";
    return true;
  }

  // finally, add the waypoints to the buffer
  for (const auto& pose : path)
    waypoint_buffer_.push_back(to_local_waypoint(pose, is_global));

  return true;
}
//}

/* localWaypointCallback //{ */
bool ControlInterface::localWaypointCallback(const std::shared_ptr<fog_msgs::srv::Vec4::Request> request,
                                             std::shared_ptr<fog_msgs::srv::Vec4::Response>      response)
{
  std::scoped_lock lck(state_mutex_, waypoint_buffer_mutex_, mission_mutex_, mission_upload_mutex_);

  // convert the single waypoint to a path containing a single point
  std::vector<std::vector<double>> path {request->goal};

  // check that we are in a state in which we can add waypoints to the buffer
  std::string reason;
  if (!addWaypoints(path, false, reason))
  {
    response->success = false;
    response->message = "Waypoints not set, " + reason;
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  response->success = true;
  response->message = "Waypoints set";
  return true;
}
//}

/* localPathCallback //{ */
bool ControlInterface::localPathCallback(const std::shared_ptr<fog_msgs::srv::Path::Request> request, std::shared_ptr<fog_msgs::srv::Path::Response> response)
{
  std::scoped_lock lck(state_mutex_, waypoint_buffer_mutex_, mission_mutex_, mission_upload_mutex_);

  // check that we are in a state in which we can add waypoints to the buffer
  std::string reason;
  if (!addWaypoints(request->path.poses, false, reason))
  {
    response->success = false;
    response->message = "Waypoints not set, " + reason;
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  response->success = true;
  response->message = "Waypoints set";
  return true;
}
//}

/* gpsWaypointCallback //{ */
bool ControlInterface::gpsWaypointCallback(const std::shared_ptr<fog_msgs::srv::Vec4::Request> request,
                                           std::shared_ptr<fog_msgs::srv::Vec4::Response>      response)
{
  std::scoped_lock lck(state_mutex_, waypoint_buffer_mutex_, mission_mutex_, mission_upload_mutex_, coord_transform_mutex_);

  // convert the single waypoint to a path containing a single point
  std::vector<std::vector<double>> path {request->goal};

  // check that we are in a state in which we can add waypoints to the buffer
  std::string reason;
  if (!addWaypoints(path, true, reason))
  {
    response->success = false;
    response->message = "Waypoints not set, " + reason;
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  response->success = true;
  response->message = "Waypoints set";
  return true;
}
//}

/* gpsPathCallback //{ */
bool ControlInterface::gpsPathCallback(const std::shared_ptr<fog_msgs::srv::Path::Request> request, std::shared_ptr<fog_msgs::srv::Path::Response> response)
{
  std::scoped_lock lck(state_mutex_, waypoint_buffer_mutex_, mission_mutex_, mission_upload_mutex_, coord_transform_mutex_);

  // check that we are in a state in which we can add waypoints to the buffer
  std::string reason;
  if (!addWaypoints(request->path.poses, true, reason))
  {
    response->success = false;
    response->message = "Waypoints not set, " + reason;
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  response->success = true;
  response->message = "Waypoints set";
  return true;
}
//}

/* waypointToLocalCallback (gps -> local frame) //{ */
bool ControlInterface::waypointToLocalCallback(const std::shared_ptr<fog_msgs::srv::WaypointToLocal::Request> request,
                                               std::shared_ptr<fog_msgs::srv::WaypointToLocal::Response>      response)
{
  std::scoped_lock lock(state_mutex_, coord_transform_mutex_);

  if (!is_initialized_)
  {
    response->success = false;
    response->message = "Cannot transform coordinates, not initialized";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  if (!gps_origin_set_)
  {
    response->success = false;
    response->message = "Cannot transform coordinates, missing GPS origin";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  const local_waypoint_t local = to_local_waypoint(*request, true);
  response->local_x = local.x;
  response->local_y = local.x;
  response->local_z = local.x;
  response->yaw = local.yaw;

  std::stringstream ss;
  ss << "Transformed GPS [" << request->latitude_deg << ", " << request->longitude_deg << ", " << request->relative_altitude_m << "] into local: ["
     << response->local_x << ", " << response->local_y << ", " << response->local_z << "]";
  response->message = ss.str();
  response->success = true;
  RCLCPP_INFO(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
  return true;
}
//}

/* pathToLocalCallback (gps -> local frame) //{ */
bool ControlInterface::pathToLocalCallback(const std::shared_ptr<fog_msgs::srv::PathToLocal::Request> request,
                                           std::shared_ptr<fog_msgs::srv::PathToLocal::Response>      response)
{
  std::scoped_lock lock(state_mutex_, coord_transform_mutex_);

  if (!is_initialized_)
  {
    response->success = false;
    response->message = "Cannot transform coordinates, not initialized";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  if (!gps_origin_set_)
  {
    response->success = false;
    response->message = "Cannot transform coordinates, GPS origin not set";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  nav_msgs::msg::Path local_path;
  for (const auto &pose : request->path.poses)
  {
    const local_waypoint_t wp = to_local_waypoint(pose, true);

    geometry_msgs::msg::Point p_out;
    p_out.x = wp.x;
    p_out.y = wp.y;
    p_out.z = wp.z;

    std::stringstream ss;
    ss << "Transformed GPS [" << pose.pose.position.x << ", " << pose.pose.position.y << "] into local: [" << p_out.x << ", " << p_out.y << "]";
    response->message = ss.str();
    response->success = true;
    RCLCPP_INFO(get_logger(), "[%s]: %s", get_name(), response->message.c_str());

    geometry_msgs::msg::PoseStamped p_stamped;
    p_stamped.pose.position    = p_out;
    p_stamped.pose.orientation = pose.pose.orientation;
    local_path.poses.push_back(p_stamped);
    local_path.header.frame_id = "local";
    local_path.header.stamp    = get_clock()->now();
  }

  std::stringstream ss;
  ss << "Transformed " << request->path.poses.size() << " GPS poses into " << response->path.poses.size() << " local poses";
  response->path    = local_path;
  response->success = true;
  response->message = ss.str();
  RCLCPP_INFO(get_logger(), "[%s]: %s", get_name(), response->message.c_str());

  return true;
}
//}

/* setPx4ParamIntCallback //{ */
bool ControlInterface::setPx4ParamIntCallback([[maybe_unused]] const std::shared_ptr<fog_msgs::srv::SetPx4ParamInt::Request> request,
                                              std::shared_ptr<fog_msgs::srv::SetPx4ParamInt::Response>                       response)
{
  std::scoped_lock lock(state_mutex_, param_mutex_);

  if (!is_initialized_)
  {
    response->success = false;
    response->message = "Failed to set PX4 parameter: not initialized";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  response->param_name = request->param_name;
  response->value      = request->value;

  const auto result = param_->set_param_int(request->param_name, request->value);
  response->success = result == mavsdk::Param::Result::Success;
  if (response->success)
  {
    response->message = "PX4 parameter successfully set";
    RCLCPP_INFO(get_logger(), "[%s]: PX4 parameter %s successfully set to %ld", get_name(), request->param_name.c_str(), request->value);
  }
  else
  {
    response->message = "Failed to set PX4 parameter: " + to_string(result);
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
  }

  return true;
}
//}

/* getPx4ParamIntCallback //{ */
bool ControlInterface::getPx4ParamIntCallback([[maybe_unused]] const std::shared_ptr<fog_msgs::srv::GetPx4ParamInt::Request> request,
                                              std::shared_ptr<fog_msgs::srv::GetPx4ParamInt::Response>                       response)
{
  std::scoped_lock lock(state_mutex_, param_mutex_);

  if (!is_initialized_)
  {
    response->success = false;
    response->message = "Failed to read PX4 parameter: not initialized";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  response->param_name = request->param_name;

  const auto [result, val] = param_->get_param_int(request->param_name);
  response->success = result == mavsdk::Param::Result::Success;
  if (response->success)
  {
    response->message = "PX4 parameter successfully read";
    response->value      = val;
    RCLCPP_INFO(get_logger(), "[%s]: PX4 parameter %s successfully read with value %ld", get_name(), request->param_name.c_str(), response->value);
  }
  else
  {
    response->message = "Failed to read PX4 parameter: " + to_string(result);
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
  }

  return true;
}
//}

/* setPx4ParamFloatCallback //{ */
bool ControlInterface::setPx4ParamFloatCallback([[maybe_unused]] const std::shared_ptr<fog_msgs::srv::SetPx4ParamFloat::Request> request,
                                                std::shared_ptr<fog_msgs::srv::SetPx4ParamFloat::Response>                       response)
{
  std::scoped_lock lock(state_mutex_, param_mutex_);

  if (!is_initialized_)
  {
    response->success = false;
    response->message = "Parameter cannot be set, not initialized";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  response->param_name = request->param_name;
  response->value      = request->value;

  const auto result = param_->set_param_float(request->param_name, request->value);
  response->success = result == mavsdk::Param::Result::Success;
  if (response->success)
  {
    response->message = "PX4 parameter successfully set";
    RCLCPP_INFO(get_logger(), "[%s]: PX4 parameter %s successfully set to %f", get_name(), request->param_name.c_str(), request->value);
  }
  else
  {
    response->message = "Failed to set PX4 parameter: " + to_string(result);
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
  }

  return true;
}
//}

/* getPx4ParamFloatCallback //{ */
bool ControlInterface::getPx4ParamFloatCallback([[maybe_unused]] const std::shared_ptr<fog_msgs::srv::GetPx4ParamFloat::Request> request,
                                                std::shared_ptr<fog_msgs::srv::GetPx4ParamFloat::Response>                       response)
{
  std::scoped_lock lock(state_mutex_, param_mutex_);

  if (!is_initialized_)
  {
    response->success = false;
    response->message = "Failed to read PX4 parameter: not initialized";
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
    return true;
  }

  response->param_name = request->param_name;

  const auto [result, val] = param_->get_param_float(request->param_name);
  response->success = result == mavsdk::Param::Result::Success;
  if (response->success)
  {
    response->message = "PX4 parameter successfully read";
    response->value      = val;
    RCLCPP_INFO(get_logger(), "[%s]: PX4 parameter %s successfully read with value %f", get_name(), request->param_name.c_str(), response->value);
  }
  else
  {
    response->message = "Failed to read PX4 parameter: " + to_string(result);
    RCLCPP_ERROR(get_logger(), "[%s]: %s", get_name(), response->message.c_str());
  }

  return true;
}
//}

/* mavsdkConnectionRoutine //{ */
void ControlInterface::mavsdkConnectionRoutine()
{
  RCLCPP_INFO(get_logger(), "[%s]: Systems size: %ld", get_name(), mavsdk_.systems().size());
  if (mavsdk_.systems().empty())
  {
    RCLCPP_INFO(get_logger(), "[%s]: Waiting for connection at URL: %s", get_name(), device_url_.c_str());
    return;
  }

  // use the first connected system
  system_ = mavsdk_.systems().front();
  RCLCPP_INFO(get_logger(), "[%s]: ID: %u", get_name(), system_->get_system_id());

  // setup the other mavsdk connections
  {
    std::scoped_lock lck(action_mutex_, mission_mutex_, param_mutex_);
    RCLCPP_INFO(get_logger(), "[%s]: Target connected", get_name());
    action_  = std::make_shared<mavsdk::Action>(system_);
    mission_ = std::make_shared<mavsdk::Mission>(system_);
    param_   = std::make_shared<mavsdk::Param>(system_);
  }

  // set default parameters to PX4
  auto request = std::make_shared<fog_msgs::srv::SetPx4ParamFloat::Request>();
  auto response = std::make_shared<fog_msgs::srv::SetPx4ParamFloat::Response>();

  request->param_name = "NAV_ACC_RAD";
  request->value      = waypoint_acceptance_radius_;
  RCLCPP_INFO(get_logger(), "[%s]: Setting %s, value: %f", get_name(), request->param_name.c_str(), request->value);
  setPx4ParamFloatCallback(request, response);

  request->param_name = "NAV_LOITER_RAD";
  request->value      = waypoint_acceptance_radius_;
  RCLCPP_INFO(get_logger(), "[%s]: Setting %s, value: %f", get_name(), request->param_name.c_str(), request->value);
  setPx4ParamFloatCallback(request, response);

  request->param_name = "NAV_MC_ALT_RAD";
  request->value      = altitude_acceptance_radius_;
  RCLCPP_INFO(get_logger(), "[%s]: Setting %s, value: %f", get_name(), request->param_name.c_str(), request->value);
  setPx4ParamFloatCallback(request, response);

  mavsdk_connection_timer_->cancel();

  // finally, set the initialized flag to true
  std::scoped_lock lck(state_mutex_);
  is_initialized_ = true;
  RCLCPP_INFO(get_logger(), "[%s]: Initialized", get_name());
}
//}

/* controlRoutine //{ */
void ControlInterface::controlRoutine()
{
  std::scoped_lock lck(state_mutex_);

  if (!is_initialized_)
    return;

  publishDiagnostics();
  publishDesiredPose();

  /* check that the mission may be updated (we're flying and no manual override) //{ */
  
  if (!gps_origin_set_ || !getting_odom_)
  {
    RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 1000, "[%s]: GPS origin set: %s, Getting odometry: %s", get_name(),
                         gps_origin_set_.load() ? "TRUE" : "FALSE", getting_odom_.load() ? "TRUE" : "FALSE");
    return;
  }
  
  RCLCPP_INFO_ONCE(get_logger(), "[%s]: CONTROL INTERFACE IS READY", get_name());
  
  if (!armed_)
  {
    RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 1000, "[%s]: Vehicle not armed", get_name());
    return;
  }
  
  if (landed_)
  {
    RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 1000, "[%s]: Vehicle not airborne", get_name());
    return;
  }
  
  if (manual_override_)
  {
    RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 1000, "[%s]: Control action prevented by manual override", get_name());
    return;
  }
  
  //}

  // update the mission (load new waypoints etc.)
  switch (mission_state_)
  {
    case mission_state_t::finished:
      state_mission_finished(); break;
    case mission_state_t::uploading:
      state_mission_uploading(); break;
    case mission_state_t::in_progress:
      state_mission_in_progress(); break;
  }
}
//}

// | -------- Implementation of mission control states -------- |

/* ControlInterface::state_mission_finished() //{ */
void ControlInterface::state_mission_finished()
{
  const std::scoped_lock lock(waypoint_buffer_mutex_, mission_mutex_, mission_upload_mutex_);
  // create a new mission plan if there are unused points in the buffer
  if (!waypoint_buffer_.empty())
  {
    publishDebugMarkers();
    RCLCPP_INFO(get_logger(), "[%s]: Waypoints to be visited: %ld", get_name(), waypoint_buffer_.size());
    // pause the current mission (TODO: is this necessary?)
    mission_->pause_mission();

    // transform and move the mission waypoints buffer to the mission_upload_waypoints_ buffer with the mavsdk type - we'll attempt to upload the waypoint_upload_buffer_ to pixhawk
    mission_upload_waypoints_.mission_items.clear();
    // transform the points
    for (const auto& pt : waypoint_buffer_)
      mission_upload_waypoints_.mission_items.push_back(to_mission_item(pt));

    // update the current desired_pose_
    const auto& last_wp = waypoint_buffer_.back();
    desired_pose_ = Eigen::Vector4d(last_wp.x, last_wp.y, last_wp.z, last_wp.yaw);

    // clear the waypoint buffer
    waypoint_buffer_.clear();

    // start upload of the new waypoints
    mission_upload_attempts_ = 0;
    startMissionUpload(mission_upload_waypoints_); // this starts the asynchronous upload process
    mission_state_ = mission_state_t::uploading;
  }
}
//}

/* ControlInterface::state_mission_uploading() //{ */
void ControlInterface::state_mission_uploading()
{
  const std::scoped_lock lock(mission_mutex_, mission_upload_mutex_);
  switch (mission_upload_state_)
  {
    // if the mission is being uploaded, just wait for it to either fail or finish
    case mission_upload_state_t::started:
      break;

    // if the mission upload failed, either retry, or fail altogether
    case mission_upload_state_t::failed:
      if (mission_upload_attempts_ < mission_upload_attempts_threshold_)
      {
        RCLCPP_INFO(get_logger(), "[%s]: Retrying upload of %ld waypoints", get_name(), mission_upload_waypoints_.mission_items.size());
        mission_upload_attempts_++;
        startMissionUpload(mission_upload_waypoints_); // this starts the asynchronous upload process
      }
      else
      {
        mission_state_ = mission_state_t::finished;
        RCLCPP_WARN(get_logger(), "[%s]: Mission upload failed too many times. Scrapping mission.", get_name());
      }
      break;

    case mission_upload_state_t::done:
      // clear the mission finish flag and start the uploaded mission
      mission_finished_flag_ = false;
      if (startMission())
      {
        last_mission_size_ = mission_upload_waypoints_.mission_items.size();
        mission_state_ = mission_state_t::in_progress;
      }
      break;
  }
}
//}

/* ControlInterface::state_mission_in_progress() //{ */
void ControlInterface::state_mission_in_progress()
{
  const std::scoped_lock lock(mission_mutex_);
  // stop if final goal is reached
  if (mission_finished_flag_) // this flag is set from the missionResultCallback
  {
    RCLCPP_INFO(get_logger(), "[%s]: All waypoints have been visited", get_name());
    mission_state_ = mission_state_t::finished;
  }
}
//}

// | ----------- Action and mission related methods ----------- |

/* start_takeoff //{ */
// the following mutexes have to be locked by the calling function:
// state_mutex_
// pose_mutex_
// action_mutex_
bool ControlInterface::start_takeoff()
{
  if (action_->set_takeoff_altitude(takeoff_height_) != mavsdk::Action::Result::Success)
  {
    RCLCPP_ERROR(get_logger(), "[%s]: Failed to set takeoff height %.2f", get_name(), takeoff_height_);
    return false;
  }

  if (reset_octomap_before_takeoff_)
  {
    const auto reset_srv   = std::make_shared<std_srvs::srv::Empty::Request>();
    const auto call_result = octomap_reset_client_->async_send_request(reset_srv);
    RCLCPP_INFO(get_logger(), "[%s]: Resetting octomap server", get_name());
  }

  if (action_->takeoff() != mavsdk::Action::Result::Success)
  {
    RCLCPP_ERROR(get_logger(), "[%s]: Takeoff failed", get_name());
    return false;
  }

  if (pose_takeoff_samples_.size() < (size_t)takeoff_position_samples_)
  {
    RCLCPP_WARN(get_logger(), "[%s]: Takeoff rejected. Need %d odometry samples, only have %ld", get_name(), takeoff_position_samples_,
                pose_takeoff_samples_.size());
    return false;
  }

  local_waypoint_t current_goal;

  // averaging desired takeoff position
  current_goal.x = 0;
  current_goal.y = 0;

  for (const auto &p : pose_takeoff_samples_)
  {
    current_goal.x += p.x();
    current_goal.y += p.y();
  }
  current_goal.x /= pose_takeoff_samples_.size();
  current_goal.y /= pose_takeoff_samples_.size();

  current_goal.z   = takeoff_height_;
  current_goal.yaw = getYaw(pose_ori_);
  desired_pose_    = Eigen::Vector4d(current_goal.x, current_goal.y, current_goal.z, current_goal.yaw);

  waypoint_buffer_.push_back(current_goal);

  takeoff_called_ = true;
  takeoff_time_ = get_clock()->now();
  RCLCPP_INFO(get_logger(), "[%s]: Taking off", get_name());
  return true;
}
//}

/* land //{ */
// the following mutexes have to be locked by the calling function:
// state_mutex_
// action_mutex_
bool ControlInterface::land()
{
  RCLCPP_INFO(get_logger(), "[%s]: Landing called. Stop commanding", get_name());
  manual_override_ = true;
  takeoff_completed_ = false;

  if (action_->land() != mavsdk::Action::Result::Success)
  {
    RCLCPP_ERROR(get_logger(), "[%s]: Landing failed", get_name());
    return false;
  }
  RCLCPP_INFO(get_logger(), "[%s]: Landing", get_name());
  return true;
}
//}

/* startMission //{ */
// the following mutexes have to be locked by the calling function:
// state_mutex_
// mission_mutex_
bool ControlInterface::startMission()
{
  if (manual_override_)
  {
    RCLCPP_WARN(get_logger(), "[%s]: Mission start prevented by manual override", get_name());
    return false;
  }

  const auto result = mission_->start_mission();
  if (result != mavsdk::Mission::Result::Success)
  {
    RCLCPP_ERROR(get_logger(), "[%s]: Mission start rejected with exit symbol %s", get_name(), to_string(result).c_str());
    return false;
  }
  RCLCPP_INFO(get_logger(), "[%s]: Mission started", get_name());
  return true;
}
//}

/* startMissionUpload //{ */
// the following mutexes have to be locked by the calling function:
// state_mutex_
// mission_mutex_
// mission_upload_mutex_
bool ControlInterface::startMissionUpload(const mavsdk::Mission::MissionPlan& mission_plan)
{
  mission_upload_state_ = mission_upload_state_t::failed;

  if (mission_plan.mission_items.empty())
  {
    RCLCPP_ERROR(get_logger(), "[%s]: Mission waypoints empty. Nothing to upload", get_name());
    return false;
  }

  if (manual_override_)
  {
    RCLCPP_WARN(get_logger(), "[%s]: Mission upload prevented by manual override", get_name());
    return false;
  }

  const auto clear_result = mission_->clear_mission();
  if (clear_result != mavsdk::Mission::Result::Success)
  {
    RCLCPP_ERROR(get_logger(), "[%s]: Mission upload failed. Could not clear previous mission", get_name());
    return false;
  }

  mission_->upload_mission_async(mission_plan, [this](mavsdk::Mission::Result result)
      {
        std::scoped_lock lck(mission_upload_mutex_);
        if (result == mavsdk::Mission::Result::Success)
          mission_upload_state_ = mission_upload_state_t::done;
        else
          mission_upload_state_ = mission_upload_state_t::failed;
      }
    );
  mission_upload_attempts_++;
  RCLCPP_INFO(get_logger(), "[%s]: Started mission upload attempt #%d", get_name(), mission_upload_attempts_);

  mission_upload_state_ = mission_upload_state_t::started;
  return true;
}
//}

/* stopMission //{ */
// the following mutexes have to be locked by the calling function:
// waypoint_buffer_mutex_
// mission_mutex_
// mission_upload_mutex_
bool ControlInterface::stopMission()
{
  // clear 
  waypoint_buffer_.clear();
  mission_upload_waypoints_.mission_items.clear();
  mission_state_ = mission_state_t::finished;

  if (mission_state_ == mission_state_t::finished)
    return true;

  // cancel any current mission upload to pixhawk
  if (mission_->cancel_mission_upload() != mavsdk::Mission::Result::Success)
  {
    RCLCPP_ERROR(get_logger(), "[%s]: Failed to cancel current mission upload", get_name());
    return false;
  }

  // cancel any mission currently being executed
  if (mission_->clear_mission() != mavsdk::Mission::Result::Success)
  {
    RCLCPP_ERROR(get_logger(), "[%s]: Previous mission cannot be stopped", get_name());
    return false;
  }

  RCLCPP_INFO(get_logger(), "[%s]: Current mission stopped", get_name());
  return true;
}
//}

// | ---------- Diagnostics and debug helper methods ---------- |

/* publishDiagnostics //{ */
void ControlInterface::publishDiagnostics()
{
  std::scoped_lock lock(mission_mutex_, waypoint_buffer_mutex_);

  fog_msgs::msg::ControlInterfaceDiagnostics msg;
  msg.header.stamp         = get_clock()->now();
  msg.header.frame_id      = world_frame_;
  msg.armed                = armed_;
  msg.airborne             = !landed_ && takeoff_completed_;
  msg.moving               = mission_state_ == mission_state_t::in_progress;
  msg.mission_finished     = mission_state_ == mission_state_t::finished;
  msg.getting_control_mode = getting_control_mode_;
  msg.getting_land_sensor  = getting_landed_info_;
  msg.gps_origin_set       = gps_origin_set_;
  msg.getting_odom         = getting_odom_;
  msg.manual_control       = manual_override_;
  msg.last_mission_size    = last_mission_size_;

  diagnostics_publisher_->publish(msg);
}
//}

/* publishDesiredPose //{ */
void ControlInterface::publishDesiredPose()
{
  geometry_msgs::msg::PoseStamped msg;
  msg.header.stamp     = get_clock()->now();
  msg.header.frame_id  = world_frame_;
  msg.pose.position.x  = desired_pose_.x();
  msg.pose.position.y  = desired_pose_.y();
  msg.pose.position.z  = desired_pose_.z();
  const Eigen::Quaterniond q(Eigen::AngleAxisd(desired_pose_.w(), Eigen::Vector3d::UnitZ()));
  msg.pose.orientation.w = q.w();
  msg.pose.orientation.x = q.x();
  msg.pose.orientation.y = q.y();
  msg.pose.orientation.z = q.z();
  desired_pose_publisher_->publish(msg);
}
//}

/* publishDebugMarkers //{ */
void ControlInterface::publishDebugMarkers()
{
  geometry_msgs::msg::PoseArray msg;
  msg.header.stamp    = get_clock()->now();
  msg.header.frame_id = world_frame_;

  for (auto &w : waypoint_buffer_) {
    geometry_msgs::msg::Pose p;
    p.position.x = w.x;
    p.position.y = w.y;
    p.position.z = w.z;
    const Eigen::Quaterniond q(Eigen::AngleAxisd(w.yaw, Eigen::Vector3d::UnitZ()));
    p.orientation.w = q.w();
    p.orientation.x = q.x();
    p.orientation.y = q.y();
    p.orientation.z = q.z();
    msg.poses.push_back(p);
  }
  waypoint_publisher_->publish(msg);
}
//}

// | --------------------- Utility methods -------------------- |

/* parse_param //{ */
template <class T>
bool ControlInterface::parse_param(const std::string &param_name, T &param_dest)
{
  declare_parameter<T>(param_name);
  if (!get_parameter(param_name, param_dest))
  {
    RCLCPP_ERROR(get_logger(), "[%s]: Could not load param '%s'", get_name(), param_name.c_str());
    return false;
  }
  else
  {
    RCLCPP_INFO_STREAM(get_logger(), "[" << get_name() << "]: Loaded '" << param_name << "' = '" << param_dest << "'");
  }
  return true;
}
//}

/* to_mission_item //{ */
mavsdk::Mission::MissionItem ControlInterface::to_mission_item(const local_waypoint_t& w_in)
{
  local_waypoint_t w = w_in;
  // apply home offset correction
  w.x -= home_position_offset_.x();
  w.y -= home_position_offset_.y();
  w.z -= home_position_offset_.z();

  mavsdk::Mission::MissionItem item;
  gps_waypoint_t               global = localToGlobal(coord_transform_, w);
  item.latitude_deg                   = global.latitude;
  item.longitude_deg                  = global.longitude;
  item.relative_altitude_m            = global.altitude;
  item.yaw_deg                        = -radToDeg(global.yaw + yaw_offset_correction_);
  item.speed_m_s                      = target_velocity_;  // NAN = use default values. This does NOT
                                                           // limit vehicle max speed
  item.is_fly_through          = true;
  item.gimbal_pitch_deg        = 0.0f;
  item.gimbal_yaw_deg          = 0.0f;
  item.camera_action           = mavsdk::Mission::MissionItem::CameraAction::None;
  item.loiter_time_s           = waypoint_loiter_time_;
  item.camera_photo_interval_s = 0.0f;
  item.acceptance_radius_m     = waypoint_acceptance_radius_;

  return item;
}
//}

/* to_local_waypoint //{ */
local_waypoint_t ControlInterface::to_local_waypoint(const geometry_msgs::msg::PoseStamped& in, const bool is_global)
{
  return to_local_waypoint(Eigen::Vector4d{in.pose.position.x, in.pose.position.y, in.pose.position.z, getYaw(in.pose.orientation)}, is_global);
}

local_waypoint_t ControlInterface::to_local_waypoint(const fog_msgs::srv::WaypointToLocal::Request& in, const bool is_global)
{
  const Eigen::Vector4d as_vec {in.latitude_deg, in.longitude_deg, in.relative_altitude_m, in.yaw};
  return to_local_waypoint(as_vec, is_global);
}

local_waypoint_t ControlInterface::to_local_waypoint(const std::vector<double>& in, const bool is_global)
{
  assert(in.size() == 4);
  const Eigen::Vector4d as_vec {in.at(0), in.at(1), in.at(2), in.at(3)};
  return to_local_waypoint(as_vec, is_global);
}

local_waypoint_t ControlInterface::to_local_waypoint(const Eigen::Vector4d& in, const bool is_global)
{
  if (is_global)
  {
    gps_waypoint_t wp;
    wp.latitude  = in.x();
    wp.longitude = in.y();
    wp.altitude  = in.z();
    wp.yaw       = in.w();
    return globalToLocal(coord_transform_, wp);
  }
  else
  {
    local_waypoint_t wp;
    wp.x   = in.x();
    wp.y   = in.y();
    wp.z   = in.z();
    wp.yaw = in.w();
    return wp;
  }
}
//}

/* to_string() function //{ */
std::string to_string(const mavsdk::Mission::Result result)
{
  switch (result)
  {
    case mavsdk::Mission::Result::Unknown:                return "Unknown result.";
    case mavsdk::Mission::Result::Success:                return "Request succeeded.";
    case mavsdk::Mission::Result::Error:                  return "Error.";
    case mavsdk::Mission::Result::TooManyMissionItems:    return "Too many mission items in the mission.";
    case mavsdk::Mission::Result::Busy:                   return "Vehicle is busy.";
    case mavsdk::Mission::Result::Timeout:                return "Request timed out.";
    case mavsdk::Mission::Result::InvalidArgument:        return "Invalid argument.";
    case mavsdk::Mission::Result::Unsupported:            return "Mission downloaded from the system is not supported.";
    case mavsdk::Mission::Result::NoMissionAvailable:     return "No mission available on the system.";
    case mavsdk::Mission::Result::UnsupportedMissionCmd:  return "Unsupported mission command.";
    case mavsdk::Mission::Result::TransferCancelled:      return "Mission transfer (upload or download) has been cancelled.";
    case mavsdk::Mission::Result::NoSystem:               return "No system connected. ";
  }
  return "Invalid result.";
}

std::string to_string(const mavsdk::Param::Result result)
{
  switch (result)
  {
    case mavsdk::Param::Result::Unknown:          return "Unknown result.";
    case mavsdk::Param::Result::Success:          return "Request succeeded.";
    case mavsdk::Param::Result::Timeout:          return "Request timed out.";
    case mavsdk::Param::Result::ConnectionError:  return "Connection error.";
    case mavsdk::Param::Result::WrongType:        return "Wrong type.";
    case mavsdk::Param::Result::ParamNameTooLong: return "Parameter name too long (> 16).";
    case mavsdk::Param::Result::NoSystem:         return "No system connected.";
  }
  return "Invalid result.";
}

std::string to_string(const mavsdk::ConnectionResult result)
{
  switch (result)
  {
    case mavsdk::ConnectionResult::Success:               return "Connection succeeded.";
    case mavsdk::ConnectionResult::Timeout:               return "Connection timed out.";
    case mavsdk::ConnectionResult::SocketError:           return "Socket error.";
    case mavsdk::ConnectionResult::BindError:             return "Bind error.";
    case mavsdk::ConnectionResult::SocketConnectionError: return "Socket connection error.";
    case mavsdk::ConnectionResult::ConnectionError:       return "Connection error.";
    case mavsdk::ConnectionResult::NotImplemented:        return "Connection type not implemented.";
    case mavsdk::ConnectionResult::SystemNotConnected:    return "No system is connected.";
    case mavsdk::ConnectionResult::SystemBusy:            return "System is busy.";
    case mavsdk::ConnectionResult::CommandDenied:         return "Command is denied.";
    case mavsdk::ConnectionResult::DestinationIpUnknown:  return "Connection IP is unknown.";
    case mavsdk::ConnectionResult::ConnectionsExhausted:  return "Connections exhausted.";
    case mavsdk::ConnectionResult::ConnectionUrlInvalid:  return "URL invalid.";
    case mavsdk::ConnectionResult::BaudrateUnknown:       return "Baudrate unknown. ";
  }
  return "Invalid result.";
}
//}

/* new_cbk_grp() method //{ */
// just a util function that returns a new mutually exclusive callback group to shorten the call
rclcpp::CallbackGroup::SharedPtr ControlInterface::new_cbk_grp()
{
  return create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
}
//}

//}

}  // namespace control_interface

#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(control_interface::ControlInterface)
