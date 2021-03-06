#include <AP_HAL.h>
#include <AP_Common.h>
#include <AP_Math.h>
#include <AP_Airspeed.h>
#include <AP_Compass.h>
#include <AP_GPS.h>
#include <AP_Compass.h>
#include <AP_Baro.h>
#include <AP_InertialSensor.h>
#include <DataFlash.h>

#include "LogReader.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "MsgHandler.h"

#define DEBUG 0
#if DEBUG
# define debug(fmt, args...)     printf(fmt "\n", ##args)
#else
# define debug(fmt, args...)
#endif

#define streq(x, y) (!strcmp(x, y))

extern const AP_HAL::HAL& hal;

LogReader::LogReader(AP_AHRS &_ahrs, AP_InertialSensor &_ins, AP_Baro &_baro, Compass &_compass, AP_GPS &_gps, AP_Airspeed &_airspeed, DataFlash_Class &_dataflash, const struct LogStructure *_structure, uint8_t _num_types) :
    vehicle(VehicleType::VEHICLE_UNKNOWN),
    ahrs(_ahrs),
    ins(_ins),
    baro(_baro),
    compass(_compass),
    gps(_gps),
    airspeed(_airspeed),
    dataflash(_dataflash),
    structure(_structure),
    num_types(_num_types),
    accel_mask(7),
    gyro_mask(7),
    last_timestamp_usec(0),
    installed_vehicle_specific_parsers(false)
{
}

struct log_Format deferred_formats[LOGREADER_MAX_FORMATS];

// some log entries (e.g. "NTUN") are used by the different vehicle
// types with wildy varying payloads.  We thus can't use the same
// parser for just any e.g. NTUN message.  We defer the registration
// of a parser for these messages until we know what model we're
// dealing with.
void LogReader::maybe_install_vehicle_specific_parsers() {
    if (! installed_vehicle_specific_parsers &&
	vehicle != VehicleType::VEHICLE_UNKNOWN) {
	switch(vehicle) {
	case VehicleType::VEHICLE_COPTER:
	    for (uint8_t i = 0; i<LOGREADER_MAX_FORMATS; i++) {
		if (deferred_formats[i].type != 0) {
		    msgparser[i] = new LR_MsgHandler_NTUN_Copter
			(deferred_formats[i], dataflash, last_timestamp_usec,
                         inavpos);
		}
	    }
	    break;
	case VehicleType::VEHICLE_PLANE:
	    break;
	case VehicleType::VEHICLE_ROVER:
	    break;
	case VehicleType::VEHICLE_UNKNOWN:
	    break;
	}
	installed_vehicle_specific_parsers = true;
    }
}

LR_MsgHandler_PARM *parameter_handler;

/*
  messages which we will be generating, so should be discarded
 */
static const char *generated_names[] = { "EKF1", "EKF2", "EKF3", "EKF4", "EKF5", 
                                         "AHR2", "POS", NULL };

/*
  see if a type is in a list of types
 */
bool LogReader::in_list(const char *type, const char *list[])
{
    for (uint8_t i=0; list[i] != NULL; i++) {
        if (strcmp(type, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

/*
  map from an incoming format type to an outgoing format type
 */
uint8_t LogReader::map_fmt_type(const char *name, uint8_t intype)
{
    if (mapped_msgid[intype] != 0) {
        // already mapped
        return mapped_msgid[intype];
    }
    // see if it is in our structure list
    for (uint8_t i=0; i<num_types; i++) {
        if (strcmp(name, structure[i].name) == 0) {
            mapped_msgid[intype] = structure[i].msg_type;
            return mapped_msgid[intype];
        }
    }
    // it is a new one, allocate an ID
    mapped_msgid[intype] = next_msgid++;
    return mapped_msgid[intype];    
}

bool LogReader::handle_log_format_msg(const struct log_Format &f) {
	char name[5];
	memset(name, '\0', 5);
	memcpy(name, f.name, 4);
	debug("Defining log format for type (%d) (%s)\n", f.type, name);

        if (!in_list(name, generated_names)) {
            /* 
               any messages which we won't be generating internally in
               replay should get the original FMT header
               We need to remap the type in the FMT header to avoid
               conflicts with our current table
            */
            struct log_Format f_mapped = f;
            f_mapped.type = map_fmt_type(name, f.type);
            dataflash.WriteBlock(&f_mapped, sizeof(f_mapped));
        }

	// map from format name to a parser subclass:
	if (streq(name, "PARM")) {
            parameter_handler = new LR_MsgHandler_PARM(formats[f.type], dataflash,
                                                    last_timestamp_usec);
	    msgparser[f.type] = parameter_handler;
	} else if (streq(name, "GPS")) {
	    msgparser[f.type] = new LR_MsgHandler_GPS(formats[f.type],
						   dataflash,
                                                   last_timestamp_usec,
                                                   gps, ground_alt_cm,
                                                   rel_altitude);
	} else if (streq(name, "GPS2")) {
	    msgparser[f.type] = new LR_MsgHandler_GPS2(formats[f.type], dataflash,
                                                    last_timestamp_usec,
						    gps, ground_alt_cm,
						    rel_altitude);
	} else if (streq(name, "MSG")) {
	    msgparser[f.type] = new LR_MsgHandler_MSG(formats[f.type], dataflash,
                                                   last_timestamp_usec,
						   vehicle, ahrs);
	} else if (streq(name, "IMU")) {
	    msgparser[f.type] = new LR_MsgHandler_IMU(formats[f.type], dataflash,
                                                   last_timestamp_usec,
						   accel_mask, gyro_mask, ins);
	} else if (streq(name, "IMU2")) {
	    msgparser[f.type] = new LR_MsgHandler_IMU2(formats[f.type], dataflash,
                                                    last_timestamp_usec,
						    accel_mask, gyro_mask, ins);
	} else if (streq(name, "IMU3")) {
	    msgparser[f.type] = new LR_MsgHandler_IMU3(formats[f.type], dataflash,
                                                    last_timestamp_usec,
						    accel_mask, gyro_mask, ins);
	} else if (streq(name, "IMT")) {
	    msgparser[f.type] = new LR_MsgHandler_IMT(formats[f.type], dataflash,
                                                      last_timestamp_usec,
                                                      accel_mask, gyro_mask, use_imt, ins);
	} else if (streq(name, "IMT2")) {
	    msgparser[f.type] = new LR_MsgHandler_IMT2(formats[f.type], dataflash,
                                                       last_timestamp_usec,
                                                       accel_mask, gyro_mask, use_imt, ins);
	} else if (streq(name, "IMT3")) {
	    msgparser[f.type] = new LR_MsgHandler_IMT3(formats[f.type], dataflash,
                                                       last_timestamp_usec,
                                                       accel_mask, gyro_mask, use_imt, ins);
	} else if (streq(name, "SIM")) {
	  msgparser[f.type] = new LR_MsgHandler_SIM(formats[f.type], dataflash,
                                                 last_timestamp_usec,
						 sim_attitude);
	} else if (streq(name, "BARO")) {
	  msgparser[f.type] = new LR_MsgHandler_BARO(formats[f.type], dataflash,
                                                  last_timestamp_usec, baro);
	} else if (streq(name, "ARM")) {
	  msgparser[f.type] = new LR_MsgHandler_ARM(formats[f.type], dataflash,
                                                  last_timestamp_usec);
	} else if (streq(name, "EV")) {
	  msgparser[f.type] = new LR_MsgHandler_Event(formats[f.type], dataflash,
                                                  last_timestamp_usec);
	} else if (streq(name, "AHR2")) {
	  msgparser[f.type] = new LR_MsgHandler_AHR2(formats[f.type], dataflash,
						  last_timestamp_usec,
                                                  ahr2_attitude);
	} else if (streq(name, "ATT")) {
	  // this parser handles *all* attitude messages - the common one,
	  // and also the rover/copter/plane-specific (old) messages
	  msgparser[f.type] = new LR_MsgHandler_ATT(formats[f.type], dataflash,
						 last_timestamp_usec,
                                                 attitude);
	} else if (streq(name, "MAG")) {
	  msgparser[f.type] = new LR_MsgHandler_MAG(formats[f.type], dataflash,
						 last_timestamp_usec, compass);
	} else if (streq(name, "MAG2")) {
	  msgparser[f.type] = new LR_MsgHandler_MAG2(formats[f.type], dataflash,
						 last_timestamp_usec, compass);
	} else if (streq(name, "NTUN")) {
	    // the label "NTUN" is used by rover, copter and plane -
	    // and they all look different!  creation of a parser is
	    // deferred until we receive a MSG log entry telling us
	    // which vehicle type to use.  Sucks.
	    memcpy(&deferred_formats[f.type], &formats[f.type],
                   sizeof(struct log_Format));
	} else if (streq(name, "ARSP")) { // plane-specific(?!)
	    msgparser[f.type] = new LR_MsgHandler_ARSP(formats[f.type], dataflash,
                                                    last_timestamp_usec,
                                                    airspeed);
	} else if (streq(name, "FRAM")) {
	    msgparser[f.type] = new LR_MsgHandler_FRAM(formats[f.type], dataflash,
                                                    last_timestamp_usec);
	} else if (streq(name, "CHEK")) {
	  msgparser[f.type] = new LR_MsgHandler_CHEK(formats[f.type], dataflash,
                                                     last_timestamp_usec,
                                                     check_state);
	} else {
            debug("  No parser for (%s)\n", name);
	}

        return true;
}

bool LogReader::handle_msg(const struct log_Format &f, uint8_t *msg) {
    char name[5];
    memset(name, '\0', 5);
    memcpy(name, f.name, 4);

    if (!in_list(name, generated_names)) {
        if (mapped_msgid[msg[2]] == 0) {
            printf("Unknown msgid %u\n", (unsigned)msg[2]);
            exit(1);
        }
        msg[2] = mapped_msgid[msg[2]];
        dataflash.WriteBlock(msg, f.length);        
        // a MsgHandler would probably have found a timestamp and
        // caled stop_clock.  This runs IO, clearing dataflash's
        // buffer.
        hal.scheduler->stop_clock(last_timestamp_usec);
    }

    LR_MsgHandler *p = msgparser[f.type];
    if (p == NULL) {
	return true;
    }

    p->process_message(msg);

    maybe_install_vehicle_specific_parsers();

    return true;
}

bool LogReader::wait_type(const char *wtype)
{
    while (true) {
        char type[5];
        if (!update(type)) {
            return false;
        }
        if (streq(type,wtype)) {
            break;
        }
    }
    return true;
}


bool LogReader::set_parameter(const char *name, float value)
{
    if (parameter_handler == NULL) {
        ::printf("No parameter format message found");
        return false;
    }
    return parameter_handler->set_parameter(name, value);
}
