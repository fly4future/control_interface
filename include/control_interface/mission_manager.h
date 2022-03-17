#ifndef MISSION_MANAGER_H
#define MISSION_MANAGER_H

/* includes //{ */

#include "control_interface/enums.h"
#include "control_interface/utils.h"

#include <fog_msgs/msg/mission_plan.hpp>

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/mission/mission.h>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp/time.hpp>

#include <mutex>

//}

namespace control_interface
{

  class MissionManager
  {
  public:
    MissionManager();
    MissionManager(const unsigned max_upload_attempts, const rclcpp::Duration& starting_timeout, 
                   std::shared_ptr<mavsdk::System> system, const rclcpp::Logger& logger, rclcpp::Clock::SharedPtr clock,
                   rclcpp::Publisher<fog_msgs::msg::MissionPlan>::SharedPtr plan_pub, std::recursive_mutex& mutex);

    // doesn't block (the uploading & starting of the mission is asynchronous)
    bool new_mission(const mavsdk::Mission::MissionPlan& mission_plan, const uint32_t id, std::string& fail_reason_out);
    // blocks until either the mission is stopped or it fails
    bool stop_mission(std::string& fail_reason_out);
    // calls stop_mission() in a new thread to avoid blocking and then calls callback with the result and reason
    void stop_mission_async(const std::function<void(bool, const std::string&)> callback);
    // some getters
    mission_state_t state();
    uint32_t mission_id();
    int32_t mission_size();
    int32_t mission_waypoint();

    // This function is called on update of mission state
    using state_update_cbk_t = std::function<void()>;
    void subscribe_state_update(const state_update_cbk_t& func);

  private:
    using state_t = mission_state_t;

    std::recursive_mutex& mutex_;

    state_update_cbk_t state_update_cbk_ = nullptr;

    // state of the current mission
    state_t state_ = state_t::finished;
    uint32_t mission_id_ = 0;

    // the MavSDK mission interface
    std::unique_ptr<mavsdk::Mission> mission_;

    // | ------------- mission upload retry variables ------------- |
    // maximal number of upload retry attempts (parameter)
    unsigned max_upload_attempts_;
    // current number of upload retry attempts
    unsigned upload_attempts_ = 0;
    // time of the last retry attempt
    rclcpp::Time last_upload_attempt_time_;

    // | -------------- mission start retry variables ------------- |
    // mission start retries will timeout after this duration (parameter)
    rclcpp::Duration starting_timeout_;
    // time of the first mission start attempt of the current mission
    rclcpp::Time first_starting_attempt_time_;
    // only used to keep track of the number of retries for printing
    unsigned starting_attempts_ = 0;

    // for printing
    rclcpp::Logger logger_;
    // for time-related stuff
    rclcpp::Clock::SharedPtr clock_;
    // for debugging
    rclcpp::Publisher<fog_msgs::msg::MissionPlan>::SharedPtr plan_pub_;

    // set by the MavSDK progressCallback
    int32_t plan_size_ = 0;
    int32_t current_waypoint_ = 0;

    // Mission upload will be retried up to a certain number of attempts.
    // This is because mission upload can take a non-negligible time (tens of ms)
    // and may fail a few times before being successful.
    void start_mission_upload(const mavsdk::Mission::MissionPlan& mission_plan);
    // Mission start will be retried repeatedly until a certain timeout duration
    // elapses. This is because mission start can be declined by PixHawk if it is
    // busy (processing the uploaded mission?). Typically, PixHawk is only busy
    // for a short time, but the mission starting is fast, so repeating it only
    // a certain number of times could fail before PixHawk is no longer busy.
    void start_mission(const mavsdk::Mission::MissionPlan& mission_plan);

    void progress_callback(const mavsdk::Mission::MissionProgress& progress);

    void update_state(const state_t new_state);

    void publish_plan(const mavsdk::Mission::MissionPlan& mission_plan);
  };

}

#endif // MISSION_MANAGER_H
