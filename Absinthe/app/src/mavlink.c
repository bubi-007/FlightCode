/*
 * mavlink.c
 *
 *  Created on: 14.07.2013
 *      Author: ��������� ��������
 */

#include "main.h"
#include "mavlink.h"
#include "common/common.h"

uint8_t system_mode = MAV_MODE_MANUAL_DISARMED; 	///< Booting up
uint32_t custom_mode = 0; 							///< Custom mode, can be defined by user/adopter
uint8_t system_state = MAV_STATE_STANDBY; 			///< System ready for flight

uint16_t m_parameter_i[2] = { 0, 0 };
static int packet_drops[2] = { 0, 0 };

extern volatile uint32_t sysTickUptime;

// waypoints.c
void wpInit(void);
void wp_message_timeout(void);
void wp_message_handler(mavlink_channel_t chan, const mavlink_message_t *msg);

// mission.c
void mp_message_handler(mavlink_channel_t chan, mavlink_message_t* msg);

#include "fifo_buffer.h"

#define	ML_RX_BUFFER_SIZE	32
static t_fifo_buffer	ml_Rx_Buffer_Hnd;
static uint8_t 			ml_Rx_Buffer[ML_RX_BUFFER_SIZE];

/* UART helper functions */
static void mlCallback(uint16_t data)
{
	fifoBuf_putByte(&ml_Rx_Buffer_Hnd, data);
}

static uint16_t mlHasData(void)
{
	return (fifoBuf_getUsed(&ml_Rx_Buffer_Hnd) == 0) ? false : true;
}

static uint8_t mlRead(void)
{
    return fifoBuf_getByte(&ml_Rx_Buffer_Hnd);
}

static void mlInit(void)
{
	mavlink_system.sysid  = cfg.mavlink_sysid; 		///< ID 20 for this UAV
	mavlink_system.compid = cfg.mavlink_compid; 	///< The component sending the message
	// TODO: Decode MW mixer type
	mavlink_system.type   = MAV_TYPE_QUADROTOR; 	///< This system is an quadrocopter.

	if (cfg.uart1_mode == UART1_MODE_MAVLINK)
	{
		fifoBuf_init(&ml_Rx_Buffer_Hnd, &ml_Rx_Buffer, ML_RX_BUFFER_SIZE);

		uartInit(MAVLINK_UART, cfg.uart1_baudrate, mlCallback);
		// TODO: Add UART1 support

	}
}

static void ml_send_25Hz(mavlink_channel_t chan)
{
	mavlink_msg_attitude_send(chan,
		sysTickUptime,					// Timestamp (milliseconds since system boot)
		angle_rad[ROLL ],				// Roll angle (rad, -pi..+pi)
		angle_rad[PITCH],				// Pitch angle (rad, -pi..+pi)
		heading_rad,	 				// Yaw angle (rad, -pi..+pi)
		gyro_sensor.data_rad[ROLL ], 	// Roll angular speed (rad/s)
		gyro_sensor.data_rad[PITCH],	// Pitch angular speed (rad/s)
		gyro_sensor.data_rad[YAW  ]		// Yaw angular speed (rad/s)
	);

	mavlink_msg_servo_output_raw_send(chan,
			sysTickUptime,		// Timestamp (milliseconds since system boot)
			1,					// Servo output port
			motor[0], motor[1], motor[2], motor[3], motor[4], motor[5], motor[6], motor[7]);
/*
	mavlink_msg_servo_output_raw_send(chan,
			sysTickUptime,						// Timestamp (milliseconds since system boot)
			2, servo[0], servo[1], servo[2], servo[3], servo[4], servo[5], servo[6], servo[7]);
*/

}

static void ml_send_10Hz(mavlink_channel_t chan)
{
	/*
	mavlink_msg_named_value_float_send(chan,
			sysTickUptime,		// Timestamp (milliseconds since system boot)
			"DEBUG_ALT",
			EstAlt
			);
	*/

	mavlink_msg_rc_channels_raw_send(chan,
			sysTickUptime,		// Timestamp (milliseconds since system boot)
			0,					// Servo output port
			rcData[0],			// RC channel 1 value, in microseconds
			rcData[1],
			rcData[2],
			rcData[3],
			rcData[4],
			rcData[5],
			rcData[6],
			rcData[7],
			rssi				// RSSI
	);
/*
	mavlink_msg_rc_channels_scaled_send(chan,
			sysTickUptime,				// Timestamp (milliseconds since system boot)
			0,							// Servo output port
			(1500 - rcData[0]) * 20,	// RC channel 1 value scaled, (-100%) -10000, (0%) 0, (100%) 10000
			(1500 - rcData[1]) * 20,
			(1500 - rcData[2]) * 20,
			(1500 - rcData[3]) * 20,
			(1500 - rcData[4]) * 20,
			(1500 - rcData[5]) * 20,
			(1500 - rcData[6]) * 20,
			(1500 - rcData[7]) * 20,
			0							// RSSI
	);
*/
	int16_t vfr_heading = heading;

    // Align to the value 0...360
    if (vfr_heading < 0)
    	vfr_heading = vfr_heading + 360;

	mavlink_msg_vfr_hud_send(chan,
			GPS_speed / 10,						// Current airspeed in m/s
			GPS_speed / 10,						// Current ground speed in m/s
			vfr_heading,						// Current heading in degrees, in compass units (0..360, 0=north)
			(rcCommand[THROTTLE] - 1000) / 10,	// Current throttle setting in integer percent, 0 to 100
			EstAlt / 100,						// Current altitude (MSL), in meters
			vario / 100							// Current climb rate in meters/second
	);
}

static void ml_send_1Hz(mavlink_channel_t chan)
{
	// Pack the message
	mavlink_msg_heartbeat_send(chan, mavlink_system.type,
		MAV_AUTOPILOT_GENERIC,
		system_mode,
		custom_mode,
		system_state
	);

	mavlink_msg_sys_status_send(chan,
/* Value of 1: present. Indices:
	0: 3D gyro,
	1: 3D acc,
	2: 3D mag,
	3: absolute pressure,
	4: differential pressure,
	5: GPS,
	6: optical flow,
	7: computer vision position,
	8: laser based position,
	9: external ground-truth (Vicon or Leica). Controllers:
	10: 3D angular rate control
	11: attitude stabilization,
	12: yaw position,
	13: z/altitude control,
	14: x/y position control,
	15: motor outputs / control. */

		// Controllers and sensors are present.
		1 << 0 |					// 3D gyro
   		1 << 1 | 					// 3D acc
   		1 << 2 |					// 3D mag
   		1 << 4 |					// differential pressure
   		(cfg.gps_baudrate ? 1 : 0) << 5 |	// GPS
   		1 << 15,					// motor outputs / control

   		// Controllers and sensors are enabled.
		1 << 0 |					// 3D gyro
   		1 << 1 | 					// 3D acc
   		1 << 2 |					// 3D mag
   		1 << 4 |					// differential pressure
   		(cfg.gps_baudrate ? 1 : 0) << 5 |	// GPS
   		1 << 15,					// motor outputs / control

   		// Controllers and sensors are operational or have an error.
		1 << 0 |					// 3D gyro
   		1 << 1 | 					// 3D acc
   		1 << 2 |					// 3D mag
   		1 << 4 |					// differential pressure
   		sensors(SENSOR_GPS) << 5 |	// GPS
   		1 << 15,					// motor outputs / control
   		cycleTime / 10,				// Maximum usage in percent of the mainloop time, (0%: 0, 100%: 1000)
		vbat * 100,					// Battery voltage, in millivolts (1 = 1 millivolt)
		ibat / 10,					// Battery current, in 10*milliamperes (1 = 10 milliampere)
		100,						// Remaining battery energy: (0%: 0, 100%: 100)
		0,							// Communication drops in percent, (0%: 0, 100%: 10'000)
		packet_drops[chan],			// Communication errors
		0,							// Autopilot-specific errors
		0,							// Autopilot-specific errors
		0,							// Autopilot-specific errors
		0							// Autopilot-specific errors
	);

//	GPS_RAW_INT (#24)
	mavlink_msg_gps_raw_int_send(chan,
		(uint64_t) micros(),
		3,						// fix_type
		GPS_coord[LAT], // 55.667248 * 1e7,		// Latitude in 1E7 degrees
		GPS_coord[LON], // 37.601129 * 1e7,		// Longitude in 1E7 degrees
		EstAlt * 10,			// Altitude in 1E3 meters (millimeters) above MSL
		GPS_hdop,				// HDOP in cm (m*100). If unknown, set to: 65535
		65535,					// VDOP in cm (m*100). If unknown, set to: 65535
		GPS_speed,				// GPS ground speed (m/s * 100). If unknown, set to: 65535
		GPS_ground_course * 10,	// Course over ground in degrees * 100, 0.0..359.99 degrees. If unknown, set to: 65535
		GPS_numSat				// satellites_visible
	);
}

static float paramGet(const param_value_t *var)
{
    union
    {
    	uint8_t		u8[4];
    	int8_t		i8[4];
    	uint16_t	u16[2];
    	int16_t		i16[2];
    	uint32_t	u32;
    	int32_t		i32;
    	float 		f;
    } value_u;

    value_u.u32 = 0;

    switch (var->type) {
    case VAR_UINT8:
        value_u.u32 = *(uint8_t *)var->ptr;
        break;

    case VAR_INT8:
        value_u.i32 = *(int8_t *)var->ptr;
        break;

    case VAR_UINT16:
        value_u.u32 = *(uint16_t *)var->ptr;
        break;

    case VAR_INT16:
        value_u.i32 = *(int16_t *)var->ptr;
        break;

    case VAR_UINT32:
        value_u.u32 = *(uint32_t *)var->ptr;
        break;

    case VAR_FLOAT:
        value_u.f = *(float *)var->ptr;
        break;
    }

	return value_u.f;
}

/**
* @brief Converter and save config parameter
*
* This function takes data from float value, convert it to appropriate type
* and store it in config parameter table.
*/
static void paramSetFromFloatValue(const param_value_t *var, float value)
{
    union
    {
    	uint8_t		u8[4];
    	int8_t		i8[4];
    	uint16_t	u16[2];
    	int16_t		i16[2];
    	uint32_t	u32;
    	float 		f;
    } value_u;

    value_u.f = value;

    switch (var->type)
    {
    case VAR_UINT8:
        *(uint8_t *) var->ptr = value_u.u8[0];
        break;

    case VAR_INT8:
        *(int8_t *) var->ptr = value_u.i8[0];
        break;

    case VAR_UINT16:
        *(uint16_t *) var->ptr = value_u.u16[0];
        break;

    case VAR_INT16:
        *(int16_t *) var->ptr = value_u.i16[0];
        break;

    case VAR_UINT32:
        *(uint32_t *) var->ptr = value_u.u32;
        break;

    case VAR_FLOAT:
		// Only write and emit changes if there is actually a difference
		// AND only write if new value is NOT "not-a-number"
		// AND is NOT infinity
		if (*(float *)var->ptr != value && !isnan(value) && !isinf(value))
		{
			*(float *) var->ptr = value;
		}
        break;
    }
}

/**
* @brief Receive communication packets and handle them
*
* This function decodes packets on the protocol level and also handles
* their value by calling the appropriate functions.
*/
static void ml_message_handler(mavlink_channel_t chan, mavlink_message_t* msg)
{
	// Handle message
	switch (msg->msgid)
	{
	case MAVLINK_MSG_ID_PARAM_REQUEST_LIST:
	{
		// Start sending parameters
		m_parameter_i[chan] = 0;
		break;
	}

	case MAVLINK_MSG_ID_HEARTBEAT:
	{
		// E.g. read GCS heartbeat and go into
		// comm lost mode if timer times out
		break;
	}


	case MAVLINK_MSG_ID_PARAM_SET:
	{
		mavlink_param_set_t set;
		mavlink_msg_param_set_decode(msg, &set);

		// Check if this message is for this system
		if ((uint8_t) set.target_system == (uint8_t) mavlink_system.sysid &&
			(uint8_t) set.target_component == (uint8_t) mavlink_system.compid)
		{
			char* key = (char*) set.param_id;

			for (uint16_t i = 0; i < valueTableCount; i++)
			{
				bool match = true;
				for (uint8_t j = 0; j < MAVLINK_MSG_PARAM_SET_FIELD_PARAM_ID_LEN; j++)
				{
					// Compare
					if (((char) (valueTable[i].name[j])) != (char) (key[j]))
					{
						match = false;
					}

					// End matching if null termination is reached
					if (((char) valueTable[i].name[j]) == '\0')
					{
						break;
					}
				}

				// Check if matched
				if (match)
				{
					paramSetFromFloatValue(&valueTable[i], set.param_value);

					// Report back new value
					mavlink_msg_param_value_send(chan,
						valueTable[i].name,
						paramGet(&valueTable[i]),
						set.param_type,
						valueTableCount, i);
				}
			}
		}
		break;
	}


	default:
		//Do nothing
	break;
	}
#if 0
	// COMMUNICATION THROUGH SECOND UART

	while (uart1_char_available())
	{
		uint8_t c = uart1_get_char();
		// Try to get a new message
		if (mavlink_parse_char(MAVLINK_COMM_1, c, &msg, &status))
		{
			// Handle message the same way like in for UART0
			// you can also consider to write a handle function like
			// handle_mavlink(mavlink_channel_t chan, mavlink_message_t* msg)
			// Which handles the messages for both or more UARTS
		}

		// And get the next one
	}

	// Update global packet drops counter
	packet_drops += status.packet_rx_drop_count;
#endif
}

/**
* @brief Send low-priority messages at a maximum rate of xx Hertz
*
* This function sends messages at a lower rate to not exceed the wireless
* bandwidth. It sends one message each time it is called until the buffer is empty.
* Call this function with xx Hertz to increase/decrease the bandwidth.
*/
static void ml_queued_send(mavlink_channel_t chan)
{
	//send parameters one by one
	if (m_parameter_i[chan] < valueTableCount)
	{
		uint par_type = MAVLINK_TYPE_FLOAT;

		switch (valueTable[m_parameter_i[chan]].type)
		{
		case VAR_FLOAT:
			par_type = MAVLINK_TYPE_FLOAT;
			break;

		case VAR_UINT8:
		case VAR_UINT16:
		case VAR_UINT32:
			par_type = MAV_PARAM_TYPE_UINT32;
			break;

		case VAR_INT8:
		case VAR_INT16:
			par_type = MAV_PARAM_TYPE_INT32;
			break;
		}

		mavlink_msg_param_value_send(chan,
			valueTable[m_parameter_i[chan]].name,
			paramGet(&valueTable[m_parameter_i[chan]]),
			par_type,
			valueTableCount,
            m_parameter_i[chan]);

		m_parameter_i[chan]++;
	}
}

/*
 * 	MAVLink protocol task
 * 	We use VCP2 and UART1 if configured
 */
portTASK_FUNCTION_PROTO(mavlinkTask, pvParameters)
{
	portTickType xLastWakeTime;
	uint8_t CycleCount1Hz = 0;
	uint8_t CycleCount10Hz = 0;
	uint8_t CycleCount25Hz = 0;
	uint16_t CycleCount5Sec = 0;

	mavlink_channel_t chan = 0;	// Use VCP2

	mlInit();
	wpInit();

	// Initialise the xLastWakeTime variable with the current time.
	xLastWakeTime = xTaskGetTickCount();

    while (1)
    {
        mavlink_message_t msg;
        mavlink_status_t status;

    	while (vcpHasData(1))	// Check if VCP2 has data?
    	{
    		uint8_t c = vcpGetByte(1);	// Get VCP2 data byte

    		if (mavlink_parse_char(chan, c, &msg, &status))
    		{
    			ml_message_handler(chan, &msg);  	// Process parameter protocol
    			mp_message_handler(chan, &msg);  	// Process mission planner command
    			wp_message_handler(chan, &msg);		// Process waypoint protocol
    		}

    		// Update global packet drops counter
        	packet_drops[chan] += status.packet_rx_drop_count;
    	}

    	if (++CycleCount25Hz == 4)
		{
			CycleCount25Hz = 0;
			ml_send_25Hz(chan);					// Send paket at 25 Hz
		}

    	if (++CycleCount10Hz == 10)
		{
			CycleCount10Hz = 0;
	    	ml_queued_send(chan);				// Send parameters at 10 Hz, if previously requested
			ml_send_10Hz(chan);					// Send paket at 10 Hz
		}

    	if (++CycleCount1Hz == 100)
		{
    		if (flag(FLAG_ARMED))
    		{
    			system_state = MAV_STATE_ACTIVE;
    			system_mode |= MAV_MODE_FLAG_SAFETY_ARMED;
    		}
    		else
    		{
    			system_state = MAV_STATE_STANDBY;
    			system_mode &= ~MAV_MODE_FLAG_SAFETY_ARMED;
    		}

			CycleCount1Hz = 0;
			ml_send_1Hz(chan);					// Send heartbeat and status paket at 1 Hz
		}

    	if (++CycleCount5Sec == 50)
		{
			CycleCount5Sec = 0;
			wp_message_timeout();				// Process waypoint protocol timeouts
		}

	    // Wait for the next cycle.
		vTaskDelayUntil(&xLastWakeTime, 10); 	// Task cycle rate 100 Hz
	}
}
