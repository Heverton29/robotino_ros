/*
 * RobotinoMotionServer.cpp
 *
 *  Created on: 14.12.2011
 *      Author: indorewala@servicerobotics.eu
 */

#include "RobotinoMotionServer.h"

#include <tf/transform_datatypes.h>

RobotinoMotionServer::RobotinoMotionServer():
	//nh_("~"),
	server_ ( nh_, "motion",
		boost::bind( &RobotinoMotionServer::execute, this, _1 ), false ),
	curr_x_( 0.0 ),
	curr_y_( 0.0 ),
	curr_phi_( 0.0 ),
	prev_phi_( 0.0 ),
	dist_moved_x_( 0.0 ),
	dist_moved_y_( 0.0 ),
	dist_rotated_( 0.0 ),
	forward_goal_x_( 0.0 ),
	forward_goal_y_( 0.0 ),
	rotation_goal_( 0.0 ),
	start_x_( 0.0 ),
	start_y_( 0.0 ),
	start_phi_( 0.0 ),
	odom_set_(false )
{
	odometry_sub_ = nh_.subscribe( "odom", 1,
			&RobotinoMotionServer::odomCallback, this );

	cmd_vel_pub_ = nh_.advertise<geometry_msgs::Twist>("cmd_vel", 1 );

	state_ = IDLE;
	movement_type_ = TRANSLATIONAL;
	task_type_ = ALIGN;
	interruption_condition_ = MOVED_DISTANCE;
	alignment_device_ = NONE;

	readParameters( nh_ );
}

RobotinoMotionServer::~RobotinoMotionServer()
{
	odometry_sub_.shutdown();
	cmd_vel_pub_.shutdown();
	server_.shutdown();
}

void RobotinoMotionServer::odomCallback( const nav_msgs::OdometryConstPtr& msg )
{
	curr_x_ = msg->pose.pose.position.x;
	curr_y_ = msg->pose.pose.position.y;
	curr_phi_ = tf::getYaw( msg->pose.pose.orientation );

	if( odom_set_ == false )
	{
		ROS_INFO( "Odometry initialized" );
		odom_set_ = true;
		prev_phi_ = curr_phi_;

		server_.start();
	}

	while( curr_phi_ - prev_phi_ < PI )
	{
		curr_phi_ += 2 * PI;
	}

	while( curr_phi_ - prev_phi_ > PI )
	{
		curr_phi_ -= 2 * PI;
	}

	dist_rotated_ += curr_phi_ - prev_phi_;
	prev_phi_ = curr_phi_;
	dist_moved_x_ = curr_x_ - start_x_;
	dist_moved_y_ = curr_y_ - start_y_;

	double distance_moved_x_temp = dist_moved_x_;

	dist_moved_x_ = dist_moved_x_ * cos( -start_phi_ ) - dist_moved_y_ * sin( -start_phi_ );
	dist_moved_y_ = dist_moved_y_ * cos( -start_phi_ ) + distance_moved_x_temp * sin( -start_phi_ );
}

void RobotinoMotionServer::execute( const robotino_motion::MotionGoalConstPtr& goal )
{
	ros::Rate loop_rate( 10 );

	if( !acceptNewGoal( goal ) )
	{
		ROS_WARN( "Goal not accepted" );
		return;
	}

	while( nh_.ok() )
	{
		if( server_.isPreemptRequested() )
		{
			if( server_.isNewGoalAvailable() )
			{
				if( acceptNewGoal( server_.acceptNewGoal() ) == false )
				{
					return;
				}
			}
			else
			{
				ROS_INFO( "Cancel request" );
				server_.setPreempted();
				state_ = IDLE;
				setCmdVel( 0, 0, 0 );

				return;
			}
		}

		controlLoop();

		if( state_ == FINISHED )
		{
			setCmdVel( 0, 0, 0 );
			cmd_vel_pub_.publish( cmd_vel_msg_ );

			result_.achieved_goal = true;
			server_.setSucceeded( result_ );
			state_ = IDLE;
			ROS_INFO( "Motion execution complete: (x[m], y[m], phi[deg]): (%f, %f, %f)",
					dist_moved_x_, dist_moved_y_, ( dist_rotated_ * 180 ) / PI );

			return;
		}

		if( state_ != IDLE )
		{
			cmd_vel_pub_.publish( cmd_vel_msg_ );
			server_.publishFeedback( feedback_ );
		}

		ros::spinOnce();
		loop_rate.sleep();
	}

	setCmdVel( 0, 0, 0 );
	cmd_vel_pub_.publish( cmd_vel_msg_ );

	server_.setAborted(robotino_motion::MotionResult(),
			"Aborting on the goal because the node has been killed");
}

void RobotinoMotionServer::setCmdVel( double vx, double vy, double omega )
{
	cmd_vel_msg_.linear.x = vx;
	cmd_vel_msg_.linear.y = vy;
	cmd_vel_msg_.angular.z = omega;
}

void RobotinoMotionServer::spin()
{
	ros::Rate loop_rate ( 5 );

	ROS_INFO( "Robotino Motion Server up and running" );
	while( nh_.ok() )
	{
		ros::spinOnce();
		loop_rate.sleep();
	}
}

void RobotinoMotionServer::controlLoop()
{
	setCmdVel(0, 0, 0);
	
	double dist_driven_x = fabs(dist_moved_x_); // current linear displacement in x axis
	double dist_driven_y = fabs(dist_moved_y_); // current linear displacement in y axis
	double dist_rotated_abs = fabs(dist_rotated_); // current angular displacement in phi axis

	double forward_goal_x_abs = fabs(forward_goal_x_);
	double forward_goal_y_abs = fabs(forward_goal_y_);
	double rotation_goal_abs = fabs(rotation_goal_);

	double distance_to_go_x = forward_goal_x_abs - dist_driven_x;
	double distance_to_go_y = forward_goal_y_abs - dist_driven_y;
	double distance_to_go_phi = rotation_goal_abs - dist_rotated_abs;

	feedback_.state.d_x = distance_to_go_x;
	feedback_.state.d_y = distance_to_go_y;
	feedback_.state.d_phi = distance_to_go_phi;

	double dist_driven = sqrt(pow(dist_driven_x, 2) + pow(dist_driven_y, 2));

	double dist_res = sqrt(pow(forward_goal_x_abs, 2) + pow(forward_goal_y_abs, 2)); // resultant linear displacement
	double ang_res = atan(forward_goal_y_abs / forward_goal_x_abs); // resultant direction

	double vel_x = 0; // linear velocity in x axis
	double vel_y = 0; // linear velocity in y axis
	double vel_phi = 0; // angular velocity in phi axis
	double vel = 0;
	
	switch (movement_type_)
	{
		case TRANSLATIONAL:
			if (dist_driven_x < forward_goal_x_abs || dist_driven_y < forward_goal_y_abs)
			{				
				ROS_DEBUG("Moved (x, y) = (%f, %f)", dist_driven_x, dist_driven_y);

				if (dist_driven >= 0 && dist_driven <= percentage_ * dist_res)
				{
					vel = sqrt(pow(min_linear_vel_, 2) + 2 * linear_acc_ * dist_driven);
				}				
				else if (dist_driven > percentage_ * dist_res && dist_driven <= (1 - percentage_) * dist_res)
				{
					vel = max_linear_vel_;
				}
				else
				{
					vel = sqrt(pow(min_linear_vel_, 2) - 2 * linear_acc_ * (dist_driven - (1 - percentage_) * dist_res));
				}

				vel_x = vel * cos(ang_res);	
				vel_y = vel * sin(ang_res);
			}
		break;
		case ROTATIONAL:
			if (dist_rotated_abs < rotation_goal_abs)
			{
				ROS_DEBUG("Rotated %f degrees", (dist_rotated_ * 180) / PI);

				vel_phi = VEL_ANG;
			}
		break;
		case TRANSLATIONAL_ROTATIONAL:	
			if (dist_driven_x < forward_goal_x_abs || dist_driven_y < forward_goal_y_abs || dist_rotated_abs < rotation_goal_abs)
			{
				ROS_DEBUG("Moved (x, y) = (%f, %f) and rotated %f degrees", dist_driven_x, dist_driven_y, (dist_rotated_ * 180) / PI);
				
				double d, alpha, phi;
				d = sqrt(pow(forward_goal_x_, 2) + pow(forward_goal_y_, 2));
				if (forward_goal_x_ != 0)
					alpha = atan(forward_goal_y_ / forward_goal_x_);
				else 
					alpha = sign(forward_goal_y_) * PI / 2;
				phi = rotation_goal_;
				ROS_INFO("d = %f, alpha = %f and phi = %f", d, (alpha * 180) / PI, (phi * 180) / PI);
				
				double V_min_temp, omega_min_temp, V_min, omega_min;				
				V_min_temp = .05 * VEL;
				omega_min_temp = .05 * VEL_ANG;
				if (phi != 0)
					V_min = omega_min_temp * d / phi;
				else
					V_min = V_min_temp;
				if (d != 0)
					omega_min = V_min_temp * phi / d;
				else 
					omega_min = omega_min_temp;
				if (V_min < V_min_temp)
					V_min = V_min_temp;
				else 
					omega_min = omega_min_temp;
				ROS_INFO("V_min = %f and omega_min = %f", V_min, (omega_min * 180) / PI);

				double V_max_temp, omega_max_temp, V_max, omega_max;
				V_max_temp = VEL;
				omega_max_temp = VEL_ANG;
				if (phi != 0)
					V_max = omega_max_temp * d / phi;	
				else
					V_max = V_max_temp;
				if (d != 0)
					omega_max = V_max_temp * phi / d;
				else
					omega_max = omega_max_temp;
				if (V_max > V_max_temp)
					V_max = V_max_temp;
				else 
					omega_max = omega_max_temp;
				ROS_INFO("V_max = %f and omega_max = %f", V_max, (omega_max * 180) / PI);

				double percentage, K, kapa;
				percentage = 20;
				if (d != 0)
					K = (V_min - V_max) / ((percentage / 100) * (1 - percentage / 100) * pow(d, 2));
				else 
					K = 0;
				if (phi != 0)				
					kapa = (omega_min - omega_max) / ((percentage / 100) * (1 - percentage / 100) * pow(phi, 2));
				else 
					kapa = 0;
				ROS_INFO("p% = %f%, K = %f and kapa = %f", percentage, K, kapa);
 
				double s, theta;
				s = sqrt(pow(dist_moved_x_, 2) + pow(dist_moved_y_, 2));
				theta = dist_rotated_;
				ROS_INFO("s = %f and theta = %f", s, (theta * 180) / PI);
				
				double vel, omega;
				if (d == 0)
					vel = 0;				
				else if (s <= d * percentage / 100 || s >= d * (1 - percentage / 100))
					vel = K * s * (s - d) + V_min;
				else 
					vel = V_max;
				if (phi == 0)
					omega = 0;
				else if (theta <= phi * percentage / 100 || theta >= phi * (1 - percentage / 100))
					omega = kapa * theta * (theta - phi) + omega_min;
				else 
					omega = omega_max;
				ROS_INFO("vel = %f and omega = %f", vel, (omega * 180) / PI);				

				vel_x = vel * cos(alpha - theta);
				vel_y = vel * sin(alpha - theta);
				vel_phi = omega;
				ROS_INFO("vel_x = %f, vel_y = %f, vel_phi = %f)", vel_y, vel_x, (vel_phi * 180) / PI);

				/*if (rotation_goal_ >=0)
				{
					if (forward_goal_x_ > 0)
					{
						vel_y = -VEL * sin(dist_rotated_abs + ang_res);
					}
					else
					{
						vel_y = VEL * sin(dist_rotated_abs + ang_res);
					}
					if (forward_goal_y_ > 0)
					{
						vel_x = -VEL * cos(dist_rotated_abs + ang_res);
					}
					else
					{
						vel_x = VEL * cos(dist_rotated_abs + ang_res);
					}
					vel_phi = rotation_goal_abs * VEL / dist_res;
				}
				else
				{
					if (forward_goal_x_ >= 0)
					{
						vel_y = VEL * sin(dist_rotated_abs + ang_res);
					}
					else
					{
						vel_y = -VEL * sin(dist_rotated_abs + ang_res);
					}
					if (forward_goal_y_ >= 0)
					{
						vel_x = VEL * cos(dist_rotated_abs + ang_res);
					}
					else
					{
						vel_x = -VEL * cos(dist_rotated_abs + ang_res);
					}
					vel_phi = rotation_goal_abs * VEL / dist_res;
				}
				*/
			}	
		break;
		case TANGENT:
			if (dist_driven_x < forward_goal_x_abs || dist_driven_y < forward_goal_y_abs || dist_rotated_abs < rotation_goal_abs)
			{
				ROS_DEBUG("Moved (x, y) = (%f, %f) and rotated %f degrees", dist_driven_x, dist_driven_y, (dist_rotated_ * 180) / PI);
	
				double radius = .5 * sqrt(pow(forward_goal_x_abs, 2) + pow(forward_goal_y_abs, 2)) / sin(rotation_goal_abs / 2);

				vel_x = VEL;
				vel_y = 0;
				vel_phi = VEL / radius;
			}	
		break;
		default:
			setCmdVel(0, 0, 0);
			return;
	}

	if (forward_goal_x_ < 0)
		vel_x = -vel_x;
	if (forward_goal_y_ < 0)
		vel_y = -vel_y;
	if(rotation_goal_ < 0)
		vel_phi = -vel_phi;

	if (vel_x == 0 && vel_y == 0 && vel_phi == 0)
		state_ = FINISHED;
	else
		setCmdVel(vel_x, vel_y, vel_phi);
}

bool RobotinoMotionServer::acceptNewGoal( const robotino_motion::MotionGoalConstPtr& goal )
{
	if( odom_set_ )
	{
		forward_goal_x_ = goal->move_x;
		forward_goal_y_ = goal->move_y;
		rotation_goal_ = goal->move_phi;
		
		switch(goal->movement_type)
		{
			case 0: 
				movement_type_ = TRANSLATIONAL;
				break;
			case 1: 
				movement_type_ = ROTATIONAL;
				break;
			case 2: 
				movement_type_ = TRANSLATIONAL_ROTATIONAL;
				break;
			case 3: 
				movement_type_ = TANGENT;
				break;
			default:
				ROS_INFO("Invalid movement_type: %d", goal->movement_type); 
				return false;
		}

		switch(goal->task_type)
		{
			case 0: 
				task_type_ = ALIGN;
				break;
			case 1: 
				task_type_ = MOVE;
				break;
			case 2: 
				task_type_ = COUNT;
				break;
			case 3: 
				task_type_ = FOLLOW;
				break;
			default:
				ROS_INFO("Invalid task_type: %d", goal->task_type); 
				return false;
		}

		switch(goal->interruption_condition)
		{
			case 0: 
				interruption_condition_ = MOVED_DISTANCE;
				break;
			case 1: 
				interruption_condition_ = HIGH_OPTICAL_SIGNAL;
				break;
			case 2: 
				interruption_condition_ = LOW_OPTICAL_SIGNAL;
				break;
			case 3: 
				interruption_condition_ = HIGH_INDUCTIVE_SIGNAL;
				break;
			case 4: 
				interruption_condition_ = LOW_INDUCTIVE_SIGNAL;
				break;
			case 5: 
				interruption_condition_ = CAMERA;
				break;
			case 6: 
				interruption_condition_ = OBSTACLE;
				break;
			case 7: 
				interruption_condition_ = BUMPER;
				break;
			case 8: 
				interruption_condition_ = TIME;
				break;
			case 9: 
				interruption_condition_ = SECURED_INFRARED_SIGNAL;
				break;
			default:
				ROS_INFO("Invalid interruption_condition: %d", goal->interruption_condition); 
				return false;
		}

		switch(goal->alignment_device)
		{
			case 0: 
				alignment_device_ = NONE;
				break;
			case 1: 
				alignment_device_ = INFRARED;
				break;
			case 2: 
				alignment_device_ = OPTICAL;
				break;
			case 3: 
				alignment_device_ = INDUCTIVE;
				break;
			case 4: 
				alignment_device_ = CAMERAAL;
				break;
			case 5: 
				alignment_device_ = ULTRASONIC;
				break;
			case 6: 
				alignment_device_ = COMPASS;
				break;			
			default:
				ROS_INFO("Invalid alignment_device: %d", goal->alignment_device); 
				return false;
		}


		ROS_INFO( "Motion execution start: (x[m], y[m], phi[deg]): (%f, %f, %f)",
				forward_goal_x_, forward_goal_y_, (rotation_goal_ * 180) / PI );

		start_x_ = curr_x_;
		start_y_ = curr_y_;
		start_phi_ = curr_phi_;
		dist_moved_x_ = 0.0;
		dist_moved_y_ = 0.0;
		dist_rotated_ = 0.0;

		if( fabs( forward_goal_x_ ) > 0.02 || fabs( forward_goal_y_ ) > 0.02 || fabs( rotation_goal_ ) > 0.01 )
		{
			state_ = MOVING;
		}
		else
		{
			state_ = FINISHED;
		}

		return true;
	}
	else
	{
		ROS_ERROR( "Odometry not initialized" );
		return false;
	}
}

void RobotinoMotionServer::readParameters( ros::NodeHandle& n)
{
	n.param( "min_linear_vel", min_linear_vel_, 0.05 );

	n.param( "max_linear_vel", max_linear_vel_, 0.1 );

	n.param( "linear_acc", linear_acc_, 0.2 );
	
	n.param( "min_angular_vel", min_angular_vel_, 0.04 );

	n.param( "max_angular_vel", max_angular_vel_, 0.2 );

	n.param( "angular_acc", angular_acc_, 0.2 );

	n.param( "percentage", percentage_, 0.1);

}

