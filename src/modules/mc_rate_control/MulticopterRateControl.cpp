/****************************************************************************
 *
 *   Copyright (c) 2013-2019 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "MulticopterRateControl.hpp"

#include <drivers/drv_hrt.h>
#include <circuit_breaker/circuit_breaker.h>
#include <mathlib/math/Limits.hpp>
#include <mathlib/math/Functions.hpp>

using namespace matrix;
using namespace time_literals;
using math::radians;

MulticopterRateControl::MulticopterRateControl(bool vtol) :
	ModuleParams(nullptr),
	WorkItem(MODULE_NAME, px4::wq_configurations::rate_ctrl),
	_actuators_0_pub(vtol ? ORB_ID(actuator_controls_virtual_mc) : ORB_ID(actuator_controls_0)),
	_loop_perf(perf_alloc(PC_ELAPSED, MODULE_NAME": cycle"))
{
	_vehicle_status.vehicle_type = vehicle_status_s::VEHICLE_TYPE_ROTARY_WING;

	parameters_updated();
}

MulticopterRateControl::~MulticopterRateControl()
{
	perf_free(_loop_perf);
}

bool
MulticopterRateControl::init()
{
	if (!_vehicle_angular_velocity_sub.registerCallback()) {
		PX4_ERR("vehicle_angular_velocity callback registration failed!");
		return false;
	}

	// limit to  250 Hz
	_vehicle_angular_velocity_sub.set_interval_us(4_ms);

	_last_run = hrt_absolute_time();

	_indi_control.init();

	return true;
}

void
MulticopterRateControl::parameters_updated()
{
	// rate control parameters
	// The controller gain K is used to convert the parallel (P + I/s + sD) form
	// to the ideal (K * [1 + 1/sTi + sTd]) form
	const Vector3f rate_k = Vector3f(_param_mc_rollrate_k.get(), _param_mc_pitchrate_k.get(), _param_mc_yawrate_k.get());

	_indi_control.setParams(rate_k.emult(Vector3f(_param_mc_indiroll_p.get(), _param_mc_indipitch_p.get(), _param_mc_indiyaw_p.get())),
				_param_mc_wind_2_torque.get(), _param_mc_omega_2_wind.get());

	_rate_control.setGains(
		rate_k.emult(Vector3f(_param_mc_rollrate_p.get(), _param_mc_pitchrate_p.get(), _param_mc_yawrate_p.get())),
		rate_k.emult(Vector3f(_param_mc_rollrate_i.get(), _param_mc_pitchrate_i.get(), _param_mc_yawrate_i.get())),
		rate_k.emult(Vector3f(_param_mc_rollrate_d.get(), _param_mc_pitchrate_d.get(), _param_mc_yawrate_d.get())));

	_rate_control.setIntegratorLimit(
		Vector3f(_param_mc_rr_int_lim.get(), _param_mc_pr_int_lim.get(), _param_mc_yr_int_lim.get()));

	_rate_control.setFeedForwardGain(
		Vector3f(_param_mc_rollrate_ff.get(), _param_mc_pitchrate_ff.get(), _param_mc_yawrate_ff.get()));


	// manual rate control acro mode rate limits
	_acro_rate_max = Vector3f(radians(_param_mc_acro_r_max.get()), radians(_param_mc_acro_p_max.get()),
				  radians(_param_mc_acro_y_max.get()));

	_actuators_0_circuit_breaker_enabled = circuit_breaker_enabled_by_val(_param_cbrk_rate_ctrl.get(), CBRK_RATE_CTRL_KEY);
}

void
MulticopterRateControl::Run()
{
	if (should_exit()) {
		_vehicle_angular_velocity_sub.unregisterCallback();
		exit_and_cleanup();
		return;
	}

	perf_begin(_loop_perf);

	// Check if parameters have changed
	if (_parameter_update_sub.updated()) {
		// clear update
		parameter_update_s param_update;
		_parameter_update_sub.copy(&param_update);

		updateParams();
		parameters_updated();
	}


	if(!_actuator_outputs_sub_flag)
	{
		_actuator_outputs_sub_flag=true;
		// publish actuator controls first
		actuator_controls_s actuators{};
		actuators.timestamp = hrt_absolute_time();
		_actuators_0_pub.publish(actuators);

		indi_feedback_input_s indi_feedback_input{};
		indi_feedback_input.timestamp = actuators.timestamp;
		_indi_fb_pub.publish(indi_feedback_input);
	}

	/* run controller on gyro changes */
	vehicle_angular_velocity_s angular_velocity;
	actuator_outputs_value_s actuator_outputs_value{};

	if (_vehicle_angular_velocity_sub.update(&angular_velocity) && _actuator_outputs_sub_flag && _actuator_outputs_value_sub.update(&actuator_outputs_value)) {

		// grab corresponding vehicle_angular_acceleration immediately after vehicle_angular_velocity copy
		vehicle_angular_acceleration_s v_angular_acceleration{};
		_vehicle_angular_acceleration_sub.copy(&v_angular_acceleration);

		const hrt_abstime now = angular_velocity.timestamp_sample;

		// Guard against too small (< 0.125ms) and too large (> 20ms) dt's.
		const float dt = math::constrain(((now - _last_run) * 1e-6f), 0.000125f, 0.02f);
		_last_run = now;

		const Vector3f angular_accel{v_angular_acceleration.xyz};
		const Vector3f rates{angular_velocity.xyz};

		/* check for updates in other topics */
		_v_control_mode_sub.update(&_v_control_mode);

		if (_vehicle_land_detected_sub.updated()) {
			vehicle_land_detected_s vehicle_land_detected;

			if (_vehicle_land_detected_sub.copy(&vehicle_land_detected)) {
				_landed = vehicle_land_detected.landed;
				_maybe_landed = vehicle_land_detected.maybe_landed;
			}
		}

		_vehicle_status_sub.update(&_vehicle_status);

		if (_landing_gear_sub.updated()) {
			landing_gear_s landing_gear;

			if (_landing_gear_sub.copy(&landing_gear)) {
				if (landing_gear.landing_gear != landing_gear_s::GEAR_KEEP) {
					_landing_gear = landing_gear.landing_gear;
				}
			}
		}

		const bool manual_control_updated = _manual_control_setpoint_sub.update(&_manual_control_setpoint);

		// generate the rate setpoint from sticks?
		bool manual_rate_sp = false;

		if (_v_control_mode.flag_control_manual_enabled &&
		    !_v_control_mode.flag_control_altitude_enabled &&
		    !_v_control_mode.flag_control_velocity_enabled &&
		    !_v_control_mode.flag_control_position_enabled) {

			if (!_v_control_mode.flag_control_attitude_enabled) {
				manual_rate_sp = true;
			}

			// Check if we are in rattitude mode and the pilot is within the center threshold on pitch and roll
			//  if true then use published rate setpoint, otherwise generate from manual_control_setpoint (like acro)
			if (_v_control_mode.flag_control_rattitude_enabled) {
				manual_rate_sp =
					(fabsf(_manual_control_setpoint.y) > _param_mc_ratt_th.get()) ||
					(fabsf(_manual_control_setpoint.x) > _param_mc_ratt_th.get());
			}
		}

		if (manual_rate_sp) {
			if (manual_control_updated) {

				// manual rates control - ACRO mode
				const Vector3f man_rate_sp{
					math::superexpo(_manual_control_setpoint.y, _param_mc_acro_expo.get(), _param_mc_acro_supexpo.get()),
					math::superexpo(-_manual_control_setpoint.x, _param_mc_acro_expo.get(), _param_mc_acro_supexpo.get()),
					math::superexpo(_manual_control_setpoint.r, _param_mc_acro_expo_y.get(), _param_mc_acro_supexpoy.get())};

				_rates_sp = man_rate_sp.emult(_acro_rate_max);
				_thrust_sp = _manual_control_setpoint.z;

				// publish rate setpoint
				vehicle_rates_setpoint_s v_rates_sp{};
				v_rates_sp.roll = _rates_sp(0);
				v_rates_sp.pitch = _rates_sp(1);
				v_rates_sp.yaw = _rates_sp(2);
				v_rates_sp.thrust_body[0] = 0.0f;
				v_rates_sp.thrust_body[1] = 0.0f;
				v_rates_sp.thrust_body[2] = -_thrust_sp;
				v_rates_sp.timestamp = hrt_absolute_time();

				_v_rates_sp_pub.publish(v_rates_sp);
			}

		} else {
			// use rates setpoint topic
			vehicle_rates_setpoint_s v_rates_sp;

			if (_v_rates_sp_sub.update(&v_rates_sp)) {
				_rates_sp(0) = v_rates_sp.roll;
				_rates_sp(1) = v_rates_sp.pitch;
				_rates_sp(2) = v_rates_sp.yaw;
				_thrust_sp = -v_rates_sp.thrust_body[2];
			}
		}

		// channels[6]:  -0.808163	0.008163	0.865306
		// channels[8]:  -0.812		0.0.028         0.868
		// channels[9]:  -0.812		0.0.028         0.868
		// channels[12]: -1		-1              1
		if (_rc_channels_sub.update(&_rc_channels))
		{
			// PX4_INFO("Hello rc! 7:%f. 9:%f. 10:%f. 13:%f.", (double) _rc_channels.channels[6], (double) _rc_channels.channels[8], (double) _rc_channels.channels[9], (double) _rc_channels.channels[12]);
			if (_rc_channels.channels[6] < -0.5f)
			{
				_sin_speed_flag = true;
				_square_roll_flag = false;
				// PX4_INFO("_sin_speed_flag !");
			}
			else if (_rc_channels.channels[6] > 0.5f)
			{
				_sin_speed_flag = false;
				_square_roll_flag = true;
				// PX4_INFO("_square_cs_flag !");
			}
			else
			{
				_sin_speed_flag = false;
				_square_roll_flag = false;
			}

			if (_rc_channels.channels[12] > 0.f)
			{
				_indi_flag = true;
				// PX4_INFO("_indi_flag !");
			}
			else
			{
				_indi_flag = false;
				// PX4_INFO("PID !");
			}
		}
		// if(_square_roll_flag)
		if (_param_use_roll_disturb.get()==1)
		{
			if (!_square_roll_flag_prev)
				_add_disturb_time = hrt_absolute_time();

			if (hrt_elapsed_time(&_add_disturb_time) / 1e6f < _param_roll_disturb_time.get())
				_rates_sp(0) = _param_roll_disturb_amplitude.get();
			else if (hrt_elapsed_time(&_add_disturb_time) / 1e6f > _param_roll_disturb_time.get() && hrt_elapsed_time(&_add_disturb_time) / 1e6f < 2.f * _param_roll_disturb_time.get())
				_rates_sp(0) = -_param_roll_disturb_amplitude.get();

			// PX4_INFO("_square_roll_flag, _rates_sp: %f", (double) _rates_sp(0));
		}

		// if (_sin_speed_flag)
		if (_param_use_sin_speed.get()==1)
		{
			if (!_sin_speed_flag_prev)
				_add_sin_time = hrt_absolute_time();

			_thrust_sp=_param_speed_sin_bia.get() + _param_speed_sin_amp.get() *sin(  (2.f*3.141592653f/_param_speed_sin_t.get()) * hrt_elapsed_time(&_add_sin_time) / 1e6f);//nuttx: 0.71. SITL: 0.5
			// PX4_INFO("_sin_speed_flag, _thrust_sp: %f, time is: %f", (double) _thrust_sp, (double) (hrt_elapsed_time(&_add_sin_time) / 1e6f));
		}
		// _sin_speed_flag_prev = _sin_speed_flag;
		// _square_roll_flag_prev = _square_roll_flag;

		_sin_speed_flag_prev = _param_use_sin_speed.get();
		_square_roll_flag_prev = _param_use_roll_disturb.get();
		// PX4_INFO("_thrust_sp: %f", (double) _thrust_sp);


		// run the rate controller
		if (_v_control_mode.flag_control_rates_enabled && !_actuators_0_circuit_breaker_enabled) {

			// reset integral if disarmed
			if (!_v_control_mode.flag_armed || _vehicle_status.vehicle_type != vehicle_status_s::VEHICLE_TYPE_ROTARY_WING) {
				_rate_control.resetIntegral();
			}

			// update saturation status from mixer feedback
			if (_motor_limits_sub.updated()) {
				multirotor_motor_limits_s motor_limits;

				if (_motor_limits_sub.copy(&motor_limits)) {
					MultirotorMixer::saturation_status saturation_status;
					saturation_status.value = motor_limits.saturation_status;

					_rate_control.setSaturationStatus(saturation_status);
				}
			}

			Vector3f Nu_i;
			Vector3f att_control;
			// run rate controller
			// if (_indi_flag)	//
			if (_param_use_indi.get() == 0)
			{
				att_control = _rate_control.update(rates, _rates_sp, angular_accel, dt, _maybe_landed || _landed);
				// PX4_INFO("PID");
			}
			else
			{
				Vector3f att_control_p = _indi_control.update(rates, _rates_sp, angular_accel, dt, actuator_outputs_value, Nu_i, _maybe_landed || _landed);
				if (_param_use_control_alloc.get() == 1)
					att_control = att_control_p;
				else
					att_control = att_control_p + Nu_i;
				// PX4_INFO("INDI");
			}
			indi_feedback_input_s indi_feedback_input{};
			indi_feedback_input.indi_fb[indi_feedback_input_s::INDEX_ROLL] = PX4_ISFINITE(Nu_i(0)) ? Nu_i(0) : 0.0f;
			indi_feedback_input.indi_fb[indi_feedback_input_s::INDEX_PITCH] = PX4_ISFINITE(Nu_i(1)) ? Nu_i(1) : 0.0f;
			indi_feedback_input.indi_fb[indi_feedback_input_s::INDEX_YAW] = PX4_ISFINITE(Nu_i(2)) ? Nu_i(2) : 0.0f;
			indi_feedback_input.timestamp_sample = angular_velocity.timestamp_sample;
			indi_feedback_input.timestamp = hrt_absolute_time();
			_indi_fb_pub.publish(indi_feedback_input);

			// publish rate controller status
			rate_ctrl_status_s rate_ctrl_status{};
			_rate_control.getRateControlStatus(rate_ctrl_status);
			rate_ctrl_status.timestamp = hrt_absolute_time();
			_controller_status_pub.publish(rate_ctrl_status);

			// publish actuator controls
			actuator_controls_s actuators{};
			actuators.control[actuator_controls_s::INDEX_ROLL] = PX4_ISFINITE(att_control(0)) ? att_control(0) : 0.0f;
			actuators.control[actuator_controls_s::INDEX_PITCH] = PX4_ISFINITE(att_control(1)) ? att_control(1) : 0.0f;
			actuators.control[actuator_controls_s::INDEX_YAW] = PX4_ISFINITE(att_control(2)) ? att_control(2) : 0.0f;
			actuators.control[actuator_controls_s::INDEX_THROTTLE] = PX4_ISFINITE(_thrust_sp) ? _thrust_sp : 0.0f;
			actuators.control[actuator_controls_s::INDEX_LANDING_GEAR] = _landing_gear;
			actuators.timestamp_sample = angular_velocity.timestamp_sample;

			// scale effort by battery status if enabled
			if (_param_mc_bat_scale_en.get()) {
				if (_battery_status_sub.updated()) {
					battery_status_s battery_status;

					if (_battery_status_sub.copy(&battery_status)) {
						_battery_status_scale = battery_status.scale;
					}
				}

				if (_battery_status_scale > 0.0f) {
					for (int i = 0; i < 4; i++) {
						actuators.control[i] *= _battery_status_scale;
					}
				}
			}


			actuators.timestamp = hrt_absolute_time();
			_actuators_0_pub.publish(actuators);

		} else if (_v_control_mode.flag_control_termination_enabled) {
			if (!_vehicle_status.is_vtol) {
				// publish actuator controls
				actuator_controls_s actuators{};
				actuators.timestamp = hrt_absolute_time();
				_actuators_0_pub.publish(actuators);
			}

		}
	}

	perf_end(_loop_perf);
}

int MulticopterRateControl::task_spawn(int argc, char *argv[])
{
	bool vtol = false;

	if (argc > 1) {
		if (strcmp(argv[1], "vtol") == 0) {
			vtol = true;
		}
	}

	MulticopterRateControl *instance = new MulticopterRateControl(vtol);

	if (instance) {
		_object.store(instance);
		_task_id = task_id_is_work_queue;

		if (instance->init()) {
			return PX4_OK;
		}

	} else {
		PX4_ERR("alloc failed");
	}

	delete instance;
	_object.store(nullptr);
	_task_id = -1;

	return PX4_ERROR;
}

int MulticopterRateControl::custom_command(int argc, char *argv[])
{
	return print_usage("unknown command");
}

int MulticopterRateControl::print_usage(const char *reason)
{
	if (reason) {
		PX4_WARN("%s\n", reason);
	}

	PRINT_MODULE_DESCRIPTION(
		R"DESCR_STR(
### Description
This implements the multicopter rate controller. It takes rate setpoints (in acro mode
via `manual_control_setpoint` topic) as inputs and outputs actuator control messages.

The controller has a PID loop for angular rate error.

)DESCR_STR");

	PRINT_MODULE_USAGE_NAME("mc_rate_control", "controller");
	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_ARG("vtol", "VTOL mode", true);
	PRINT_MODULE_USAGE_DEFAULT_COMMANDS();

	return 0;
}

extern "C" __EXPORT int mc_rate_control_main(int argc, char *argv[])
{
	return MulticopterRateControl::main(argc, argv);
}
