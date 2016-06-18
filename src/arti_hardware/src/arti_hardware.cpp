#include <arti_hardware/arti_hardware.h>

namespace arti_hardware
{

ArtiHardware::ArtiHardware(ros::NodeHandle nh, ros::NodeHandle private_nh): nh_(nh)
{
	cmd_sub_ = nh.subscribe<geometry_msgs::Twist>("cmd_vel", 1, boost::bind(&ArtiHardware::cmdVelCallback, this, _1));
	diff_cmd_sub_ = nh.subscribe<arti_msgs::DiffCmd>("diff_cmd_vel", 1, boost::bind(&ArtiHardware::diffCmdCallback, this, _1));

	private_nh.param("port", port_, std::string("/dev/ttyACM0"));
	private_nh.param("body_width", body_width_, 1.0);
	private_nh.param("baud_rate", baud_rate_, 9600);
	private_nh.param("serial_time_out", serial_time_out_, 100);
	private_nh.param("control_rate", control_rate_, 30.0);
	private_nh.param("odom_rate", odom_rate_, 50.0);
	private_nh.param("odom_window", odom_window_, 5);
	private_nh.param("cmd_time_out", cmd_time_out_, 0.5);
	private_nh.param("wheel_multiplier", wheel_multiplier_, 0.5);
	private_nh.param("maximum_vel", maximum_vel_, 1.0);
	private_nh.param("odom_bias", odom_bias_, 1.0);
	private_nh.param("maximum_vel", maximum_vel_, 1.0);
	private_nh.param("flip_lr", flip_lr_, false);

	ROS_INFO("Arti Hardware got port %s", port_.c_str());
	ROS_INFO("Set Serial Timeout %d ms", serial_time_out_);
	ROS_INFO("Baud Rate %d", baud_rate_);
	ROS_INFO("Control Rate %f", control_rate_);
	ROS_INFO("Command Time out is %f s", cmd_time_out_);

	serial::Timeout to = serial::Timeout::simpleTimeout(serial_time_out_);
	// serial::Timeout to(serial::Timeout::max(), serial_time_out_, serial_time_out_, serial_time_out_, serial_time_out_);
	serial_ = new serial::Serial(port_, baud_rate_, to, serial::eightbits, serial::parity_none, serial::stopbits_one, serial::flowcontrol_none);
	diff_odom_pub_ = nh.advertise<arti_msgs::DiffOdom>("/diff_odom", 1);
	odom_pub_ = nh.advertise<nav_msgs::Odometry>("odom", 1);

	ros::Duration(2).sleep();

	if (!serial_->isOpen()) {
		try
		{
			serial_->open();
			ROS_INFO("Connection established on: %s", port_.c_str());
		}
		catch (std::runtime_error ex)
		{
			ROS_FATAL("Serial port open failed: %s (%s)", ex.what(), port_.c_str());
			ros::shutdown();
		}

	}

	// test();
	// odomLoop();
	odom_thread_ = new boost::thread(boost::bind(&ArtiHardware::odomLoop, this));
	controlLoop();

}

ArtiHardware::~ArtiHardware()
{
	odom_thread_->interrupt();
	odom_thread_->join();
	try
	{
		sendMotorCmd(0, 0);
		serial_->close();
		ROS_INFO("Serial port shutting down");
	}
	catch (std::exception ex) {};
	delete serial_;
	delete odom_thread_;
}

void ArtiHardware::test()
{
	ros::Rate r(control_rate_);
	while (nh_.ok()) {
		std::string tmpStr;
		if (serial_->isOpen()) {
			try
			{
				tmpStr = serial_->readline(100);
				std::cout << tmpStr << std::endl;
			}
			catch (serial::SerialException ex) //No data received
			{
				ROS_WARN("Serial read exception: %s", ex.what());
				// continue;
			}
			catch (std::runtime_error ex)
			{
				ROS_WARN("Serial read exception: %s", ex.what());
				// continue;
			}
		} else {
			ROS_WARN("Serial is not open\n");
		}

	}
}

/**
 * @brief      the main control loop of a the hardware
 */
void ArtiHardware::controlLoop()
{
	ros::Rate r(control_rate_);
	while (nh_.ok()) {
		ros::spinOnce();
		// if it's been a long time since recelve the command, set command to zero
		double inter_time = (ros::Time::now() - cmd_time_).toSec();
		if ( inter_time > cmd_time_out_ ) {
			cmd_left_ = 0;
			cmd_right_ = 0;
			// std::cout << "inter time: " << inter_time << std::endl;
			cmd_time_ = ros::Time::now();
		}
		sendMotorCmd(cmd_left_, cmd_right_);

		r.sleep();
	}
}

void ArtiHardware::odomLoop()
{
	ROS_INFO_ONCE("Start to publish odom");
	ros::Rate r(odom_rate_);
	std::string dataStr;
	std::string tmpStr;
	int num = 0;
	int left = 0, right = 0;
	unsigned char token[1];
	while (nh_.ok()) {

		if (serial_->available()) {
			try
			{
				tmpStr = serial_->readline(20, "ODOMS,");
				if (!tmpStr.empty()) {
					if (tmpStr.back() == ',') {
						dataStr = serial_->readline(20, "ODOME\n");
						if (flip_lr_) {
							parseOdomStr(dataStr, left, right);
						} else {
							parseOdomStr(dataStr, right, left);
						}
					}
				}
			}
			catch (serial::SerialException ex) //No data received
			{
				ROS_WARN("Serial read exception: %s", ex.what());
				// continue;
			}
			catch (std::runtime_error ex)
			{
				ROS_WARN("Serial read exception: %s", ex.what());
				// continue;
			}
		}
		processOdom(left, right);

		dataStr = "";
		tmpStr = "";
		num = 0;
		r.sleep();
	}
}

void ArtiHardware::printOdom(const arti_msgs::DiffOdom& odom)
{
	std::cout << "left travel: " << odom.left_travel << " right travel: " <<
	          odom.right_travel << " left speed: " << odom.left_speed << " right speed:" << odom.right_speed
	          << std::endl;
}

void ArtiHardware::processOdom(const int& left, const int& right)
{
	arti_msgs::DiffOdom diff_odom;
	diff_odom.left_travel = left * wheel_multiplier_ * odom_bias_;
	diff_odom.right_travel = right * wheel_multiplier_;
	diff_odom.header.stamp = ros::Time::now();
	double dl, dr;
	double dvx, dwz;
	double dt = 0;
	// if there is not enough data in the stack add it to the queue
	if (diff_odom_queue_.size() == odom_window_) {
		dt = (diff_odom.header.stamp - diff_odom_queue_.front().header.stamp).toSec();
		if (dt < 0.00001) {
			return;
		}
		// std::cout << dt << std::endl;
		dl = diff_odom.left_travel - diff_odom_old_.left_travel;
		dr = diff_odom.right_travel - diff_odom_old_.right_travel;
		diff_odom.left_speed =  (diff_odom.left_travel - diff_odom_queue_.front().left_travel) / dt;
		diff_odom.right_speed = (diff_odom.right_travel - diff_odom_queue_.front().right_travel) / dt;
		diff_odom_queue_.pop();
	}
	vl_ = diff_odom.left_speed;
	vr_ = diff_odom.right_speed;
	diff_odom_old_ = diff_odom;
	diff_odom_queue_.push(diff_odom);
	// std::cout << diff_odom_queue_.size() << std::endl;
	diff_odom_pub_.publish(diff_odom);
	// pose estiamtion and publish the odometry
	LRtoDiff(dl, dr, dvx, dwz);
	// std::cout << dl << " " << dr << std::endl;
	LRtoDiff(vl_, vr_, vx_, wz_);
	integrateExact(dvx, dwz);
	nav_msgs::Odometry odom;
	odom.header.stamp = ros::Time::now();
	odom.header.frame_id = "odom";
	odom.pose.pose.position.x = px_;
	odom.pose.pose.position.y = py_;
	odom.pose.pose.orientation.z = theta_;
	odom.pose.pose.orientation.w = 1;
	odom.twist.twist.linear.x = vx_;
	odom.twist.twist.angular.z = wz_;
	odom_pub_.publish(odom);

}


bool ArtiHardware::parseOdomStr(const std::string& str, int& left, int& right)
{
	std::vector<int> ids;
	for (int i = 0; i < str.length(); i++) {
		if (str[i] == ',') {
			ids.push_back(i);
		}
	}

	if (ids.size() < 2) {
		return false;
	} else {
		std::string left_str = str.substr(0, ids[0]);
		std::string right_str = str.substr(ids[0] + 1, ids[1] - ids[0] - 1);
		// use boost instead of stoi is more fast and stable
		// std::cout<<str;
		try
		{
			left = boost::lexical_cast<int, std::string>(left_str);
			right = boost::lexical_cast<int, std::string>(right_str);
		}
		catch (boost::bad_lexical_cast ex)
		{
			return false;
		}
		return true;
	}
}

/**
 * @brief      { Send the motor command }
 *
 * @param[in]  left   The left command
 * @param[in]  right  The right command
 */
void ArtiHardware::sendMotorCmd(const double& left, const double right)
{
	if (!serial_->isOpen())
	{
		ROS_ERROR("Serial port not available");
		return;
	}

	sprintf(cmd_, "\nMOTOS,%d,%d,MOTOE\n", (int) (left * 127), (int) (right * 127));
	try
	{

		serial_->write(cmd_); //Send query
		if (left != 0.0 && right != 0.0) {
			std::cout << cmd_;
		}
	}
	catch (serial::SerialException ex)
	{
		ROS_WARN("No response to: \"%s\"", cmd_);
		return;
	}
	catch (std::runtime_error ex)
	{
		ROS_WARN("Exception while sending data, %s", ex.what());
		return;
	}
	return;

}

/**
 * @brief      { function_description }
 *
 * @param[in]  msg   The message
 */
void ArtiHardware::cmdVelCallback(const geometry_msgs::Twist::ConstPtr& msg)
{
	ROS_INFO_ONCE("Arti Hardware Get Command");
	diffToLR(msg->linear.x, msg->angular.z, cmd_left_, cmd_right_);
	thresholdVelocity();
	cmd_time_ = ros::Time::now();
}

void ArtiHardware::diffCmdCallback(const arti_msgs::DiffCmd::ConstPtr& msg)
{
	ROS_INFO_ONCE("Arti Hardware Get Diff Command");
	// diffToLR(msg->linear.x, msg->angular.z, cmd_left_, cmd_right_);
	cmd_left_ = msg->left;
	cmd_right_ = msg->right;
	thresholdVelocity();
	cmd_time_ = ros::Time::now();
}


void ArtiHardware::integrateRungeKutta2(const double& linear, const double& angular)
{
	const double direction = theta_ + angular * 0.5;

	/// Runge-Kutta 2nd order integration:
	px_       += linear * cos(direction);
	py_       += linear * sin(direction);
	theta_ += angular;
}

/**
 * \brief Other possible integration method provided by the class
 * \param linear
 * \param angular
 */
void ArtiHardware::integrateExact(const double& linear, const double& angular)
{
	if (fabs(angular) < 1e-6)
		integrateRungeKutta2(linear, angular);
	else
	{
		/// Exact integration (should solve problems when angular is zero):
		const double theta_old = theta_;
		const double r = linear / angular;
		theta_ += angular;
		px_       +=  r * (sin(theta_) - sin(theta_old));
		py_       += -r * (cos(theta_) - cos(theta_old));
	}
}


void ArtiHardware::thresholdVelocity()
{
	if (cmd_left_ > maximum_vel_) {
		cmd_left_ = maximum_vel_;
	}

	if (cmd_left_ < -maximum_vel_) {
		cmd_left_ = -maximum_vel_;
	}

	if (cmd_right_ > maximum_vel_) {
		cmd_right_ = maximum_vel_;
	}

	if (cmd_right_ < -maximum_vel_) {
		cmd_right_ = -maximum_vel_;
	}
}

void ArtiHardware::diffToLR(const double& vx, const double& wz, double& vl, double& vr)
{
	vl = vx - body_width_ / 2 * wz;
	vr = vx + body_width_ / 2 * wz;
}

void ArtiHardware::LRtoDiff(const double& vl, const double& vr, double& vx, double& wz)
{
	vx  = (vr + vl) * 0.5 ;
	wz = (vr - vl) / body_width_;
}

void ArtiHardware::setPose(const double&x, const double& y, const double& theta)
{
	px_ = x;
	py_ = y;
	theta_ = theta;
}

}  // namespace arti_hadware

int main(int argc, char *argv[])
{
	ros::init(argc, argv, "arti_base");
	ros::NodeHandle nh, private_nh("~");
	arti_hardware::ArtiHardware arti(nh, private_nh);

	// ros::spin();

	return 0;
}