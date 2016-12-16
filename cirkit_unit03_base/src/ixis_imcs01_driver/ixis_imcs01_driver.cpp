#include "ixis_imcs01_driver/ixis_imcs01_driver.h"

#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>

#include <ros/ros.h>

IxisImcs01Driver::IxisImcs01Driver(std::string port_name)
{
  size_t number_of_counters = 3;
  encoder_counts_.resize(number_of_counters);
  last_encoder_counts_.resize(number_of_counters);
  delta_encoder_counts_.resize(number_of_counters);
  state_.position.resize(number_of_counters);
  state_.velocity.resize(number_of_counters);
  state_.effort.resize(number_of_counters);
  state_.name.resize(number_of_counters);
  for (size_t i; i < number_of_counters; ++i) {
    encoder_counts_[i] = 0;
    last_encoder_counts_[i] = 0;
    delta_encoder_counts_[i] = -1;
    state_.position[i] = 0.0;
    state_.velocity[i] = 0.0;
    state_.effort[i] = 0;
  }
  state_.name[0] = "rear_right";
  state_.name[1] = "rear_left";
  state_.name[2] = "front";
  this->openPort(port_name);
}

IxisImcs01Driver::~IxisImcs01Driver()
{
  this->closePort();
}

int IxisImcs01Driver::openPort(std::string port_name)
{
  imcs01_fd_ = open(port_name.c_str(), O_RDWR);
  if(imcs01_fd_ < 0){
    ROS_ERROR_STREAM("iMCs01 Open Error : " << port_name);
    exit(-1);
  }else{
    if(ioctl(imcs01_fd_, URBTC_CONTINUOUS_READ) < 0){
      ROS_ERROR_STREAM("iMCs01 ioctl URBTC_CONTINUOUS_READ error");
      exit(-1);
    }
    if(ioctl(imcs01_fd_, URBTC_BUFREAD) < 0){
      ROS_ERROR_STREAM("iMCs01 ioctl URBTC_BUFREAD error");
      exit(-1);
    }
  }
  return 0;
}

int IxisImcs01Driver::closePort()
{
  close(imcs01_fd_);
}


int IxisImcs01Driver::update()
{
  if(read(imcs01_fd_, &received_data_, sizeof(received_data_))
     != sizeof(received_data_)){
    ROS_WARN_STREAM("iMCs01 update() read error");
    return -1;
  }else{
    this->parseEncoderTime();
    this->parseFrontEncoderCounts();
    this->parseRearEncoderCounts();
    return 0;
  }
}

int IxisImcs01Driver::parseEncoderTime()
{
  delta_encoder_time_ = (double)(received_data_.time) - last_encoder_time_;

  if(delta_encoder_time_ < 0){
    delta_encoder_time_ = 65535 - (last_encoder_time_ - received_data_.time);
  }
  delta_encoder_time_ = delta_encoder_time_ / 1000.0; // [ms] -> [s]
  last_encoder_time_ = (double)(received_data_.time);
  return 0;
}

int IxisImcs01Driver::parseFrontEncoderCounts()
{
  int steer_encoder_counts = (int)received_data_.ct[1];
  state_.position[2] = steer_encoder_counts*M_PI*67.0/3633.0/180.0;
  // cirkit_unit03_driverでの実装だとここで移動平均をしてる
  return 0;
}

int IxisImcs01Driver::parseRearEncoderCounts()
{
  int rear_encoder_counts[2]{(int)received_data_.ct[2], -(int)reveived_data_.ct[3]};

  for(int i = 0; i < 2; ++i){
    if(rear_delta_encoder_counts_[i] == -1
       || rear_encoder_counts_[i] == rear_last_encoder_counts_[i]){ // First time.

     rear_delta_encoder_counts_[i] = 0;

    }else{
      rear_delta_encoder_counts_[i] = rear_encoder_counts_[i] - rear_last_encoder_counts_[i];

      // checking iMCs01 counter overflow.
      if(rear_delta_encoder_counts_[i] > ROBOT_MAX_ENCODER_COUNTS/10){
        rear_delta_encoder_counts_[i] = rear_delta_encoder_counts_[i] - ROBOT_MAX_ENCODER_COUNTS;
      }
      if(rear_delta_encoder_counts_[i] < -ROBOT_MAX_ENCODER_COUNTS/10){
        rear_delta_encoder_counts_[i] = rear_delta_encoder_counts_[i] + ROBOT_MAX_ENCODER_COUNTS;
      }
    }
    rear_last_encoder_counts_[i] = rear_encoder_counts_[i];

    // Update rear state
    // pulse rate : 40.0, gear_rate : 33.0
    // position = (counts / (40.0 * 33.0)) * pi = counts * 0.00237999...
    state_.position[i] += delta_encoder_counts_[i]*0.0238;
    state_.velocity[i] = (delta_encoder_counts_[i]*0.0238/delta_encoder_time_);
  }

  return 0;
}

sensor_msgs::JointState IxisImcs01Driver::getJointState()
{
  return state_;
}

int IxisImcs01Driver::controlRearWheel(double rear_speed)
{
  static int forward_stop_cnt = 0;
  static int back_stop_cnt = 0;
  static int max_spped_x = 3.0;
  static double average_spped_x = 0;

  unsigned short duty = 0;

  // Forward
  if (rear_speed >= 0.0){
    double rear_speed_m_s = MIN(rear_speed, MAX_LIN_VEL); // return smaller
    if (running_state_ == RunningState::FORWARD
        || running_state_ == RunningState::FORWARD_STOP){
      // Now Forwarding
      average_spped_x = (average_spped_x + rear_speed)/2.0;
      u = (unsigned short)(32767.0 + 32767.0 * average_spped_x *1.0);
      if (rear_speed == 0.0){
        u = 32767;
        average_spped_x = 0;
      }
      duty = MIN(u, 60000);
      duty = MAX(duty, 32767);
      this->writeOffsetCmd(RunningMode::FORWARD, duty);
      running_state_ = RunningState::FORWARD;
    }else{
      // Now Backing
      // Need to stop once.
      duty = 32767; // STOP
      this->writeOffsetCmd(RunningMode::FORWARD, duty);
      average_spped_x = 0;

      if (forward_stop_cnt >= 20){
        running_state_ = RunningState::FORWARD_STOP;
        forward_stop_cnt = 0;
        for (int i = 0; i < 10; ++i){
          duty = 32767;
          this->writeOffsetCmd(RunningMode::FORWARD, duty);
        }
      }else{
        running_state_ = RunningState::OTHERWISE;
        forward_stop_cnt++;
      }
    }
  }else{
    // (rear_speed < 0) -> Back
    average_spped_x = 0;
    if (running_state_ == RunningState::BACK_STOP ||
        running_state_ == RunningState::BACK){
      // Now backing
      duty = 60000; // Back is constant speed
      this->writeOffsetCmd(RunningMode::Back, duty);
      running_state_ = RunningState::BACK;
      ROS_INFO("ROBOT_STASIS_BACK");
    }else{
      // Now forwarding
      if (back_stop_cnt >= 10){
        running_state_ = RunningState::BACK_STOP;
        back_stop_cnt = 0;
        duty = 32767; // STOP
        this->writeOffsetCmd(RunningMode::Back, duty);
        for (int i = 0; i < 300; ++i){ usleep(1000); }
        ROS_INFO("ROBOT_STASIS_BACK_STOP");
      }else{
        usleep(50000);
        duty = 32767; // STOP
        this->writeOffsetCmd(RunningMode::FORWARD, duty);
        running_state_ = RunningState::OTHERWISE;
        back_stop_cnt++;
        ROS_INFO("ROBOT_STASIS_OTHERWISE");
      }
    }
  }
}

int IxisImcs01Driver::writeOffsetCmd(RunningMode mode,
                                     unsigned short duty)
{
  switch (mode) {
    case RunningMode::FORWARD : {
      cmd_ccmd_.offset[0] = 65535;
      break;
    }
    case RunningMode::BACK : {
      cmd_ccmd__.offset[1] = 32767;
      break
    }
    default:
      break;
  }
  cmd_ccmd_.offset[1] = duty;
  std::lock_guard<std::mutex> lck {communication_mutex_};
  if (write(fd_imcs01, &cmd, sizeof(cmd)) < 0){
    ROS_ERROR_STREAM("iMCs01 write fail.");
    return -1;
  }else{
    return 0;
  }
}
