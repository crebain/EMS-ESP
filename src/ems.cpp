/**
 * ems.cpp
 *
 * Handles all the processing of the EMS messages
 *
 * Paul Derbyshire - https://github.com/proddy/EMS-ESP
 */

#include "ems.h"
#include "MyESP.h"
#include "ems_devices.h"
#include "ems_utils.h"
#include "emsuart.h"
#include <CircularBuffer.h> // https://github.com/rlogiacco/CircularBuffer
#include <map>

#ifdef TESTS
#include "test_data.h"
uint8_t _TEST_DATA_max = ArraySize(TEST_DATA);
#endif

_EMS_Sys_Status                                            EMS_Sys_Status; // EMS Status
CircularBuffer<_EMS_TxTelegram, EMS_TX_TELEGRAM_QUEUE_MAX> EMS_TxQueue;    // FIFO queue for Tx send buffer
std::list<_Detected_Device>                                Devices;        // for storing all detected EMS devices

uint8_t _EMS_Devices_max       = ArraySize(EMS_Devices);
uint8_t _EMS_Devices_Types_max = ArraySize(EMS_Devices_Types);

// these structs contain the data we store from the specific EMS devices
_EMS_Boiler      EMS_Boiler;      // for boiler
_EMS_Thermostat  EMS_Thermostat;  // for thermostat
_EMS_SolarModule EMS_SolarModule; // for solar modules
_EMS_HeatPump    EMS_HeatPump;    // for heatpumps
_EMS_Mixing      EMS_Mixing;      // for mixing devices

// CRC lookup table with poly 12 for faster checking
const uint8_t ems_crc_table[] = {0x00, 0x02, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x10, 0x12, 0x14, 0x16, 0x18, 0x1A, 0x1C, 0x1E, 0x20, 0x22, 0x24, 0x26,
                                 0x28, 0x2A, 0x2C, 0x2E, 0x30, 0x32, 0x34, 0x36, 0x38, 0x3A, 0x3C, 0x3E, 0x40, 0x42, 0x44, 0x46, 0x48, 0x4A, 0x4C, 0x4E,
                                 0x50, 0x52, 0x54, 0x56, 0x58, 0x5A, 0x5C, 0x5E, 0x60, 0x62, 0x64, 0x66, 0x68, 0x6A, 0x6C, 0x6E, 0x70, 0x72, 0x74, 0x76,
                                 0x78, 0x7A, 0x7C, 0x7E, 0x80, 0x82, 0x84, 0x86, 0x88, 0x8A, 0x8C, 0x8E, 0x90, 0x92, 0x94, 0x96, 0x98, 0x9A, 0x9C, 0x9E,
                                 0xA0, 0xA2, 0xA4, 0xA6, 0xA8, 0xAA, 0xAC, 0xAE, 0xB0, 0xB2, 0xB4, 0xB6, 0xB8, 0xBA, 0xBC, 0xBE, 0xC0, 0xC2, 0xC4, 0xC6,
                                 0xC8, 0xCA, 0xCC, 0xCE, 0xD0, 0xD2, 0xD4, 0xD6, 0xD8, 0xDA, 0xDC, 0xDE, 0xE0, 0xE2, 0xE4, 0xE6, 0xE8, 0xEA, 0xEC, 0xEE,
                                 0xF0, 0xF2, 0xF4, 0xF6, 0xF8, 0xFA, 0xFC, 0xFE, 0x19, 0x1B, 0x1D, 0x1F, 0x11, 0x13, 0x15, 0x17, 0x09, 0x0B, 0x0D, 0x0F,
                                 0x01, 0x03, 0x05, 0x07, 0x39, 0x3B, 0x3D, 0x3F, 0x31, 0x33, 0x35, 0x37, 0x29, 0x2B, 0x2D, 0x2F, 0x21, 0x23, 0x25, 0x27,
                                 0x59, 0x5B, 0x5D, 0x5F, 0x51, 0x53, 0x55, 0x57, 0x49, 0x4B, 0x4D, 0x4F, 0x41, 0x43, 0x45, 0x47, 0x79, 0x7B, 0x7D, 0x7F,
                                 0x71, 0x73, 0x75, 0x77, 0x69, 0x6B, 0x6D, 0x6F, 0x61, 0x63, 0x65, 0x67, 0x99, 0x9B, 0x9D, 0x9F, 0x91, 0x93, 0x95, 0x97,
                                 0x89, 0x8B, 0x8D, 0x8F, 0x81, 0x83, 0x85, 0x87, 0xB9, 0xBB, 0xBD, 0xBF, 0xB1, 0xB3, 0xB5, 0xB7, 0xA9, 0xAB, 0xAD, 0xAF,
                                 0xA1, 0xA3, 0xA5, 0xA7, 0xD9, 0xDB, 0xDD, 0xDF, 0xD1, 0xD3, 0xD5, 0xD7, 0xC9, 0xCB, 0xCD, 0xCF, 0xC1, 0xC3, 0xC5, 0xC7,
                                 0xF9, 0xFB, 0xFD, 0xFF, 0xF1, 0xF3, 0xF5, 0xF7, 0xE9, 0xEB, 0xED, 0xEF, 0xE1, 0xE3, 0xE5, 0xE7};

const uint8_t  TX_WRITE_TIMEOUT_COUNT = 2;       // 3 retries before timeout
const uint32_t EMS_BUS_TIMEOUT        = 15000;   // timeout in ms before recognizing the ems bus is offline (15 seconds)
const uint32_t EMS_POLL_TIMEOUT       = 5000000; // timeout in microseconds before recognizing the ems bus is offline (5 seconds)

/*
 * Add one or more flags to the current flags.
 */
void ems_Device_add_flags(unsigned int flags) {
    EMS_Sys_Status.emsRefreshedFlags |= flags;
}
/*
 * Check if the current flags include all of the specified flags.
 */
bool ems_Device_has_flags(unsigned int flags) {
    return (EMS_Sys_Status.emsRefreshedFlags & flags) == flags;
}
/*
 * Remove one or more flags from the current flags.
 */
void ems_Device_remove_flags(unsigned int flags) {
    EMS_Sys_Status.emsRefreshedFlags &= ~flags;
}

// init stats and counters and buffers
void ems_init() {
    ems_clearDeviceList(); // init the device map

    // overall status
    EMS_Sys_Status.emsRxPgks         = 0;
    EMS_Sys_Status.emsTxPkgs         = 0;
    EMS_Sys_Status.emxCrcErr         = 0;
    EMS_Sys_Status.emsRxStatus       = EMS_RX_STATUS_IDLE;
    EMS_Sys_Status.emsTxStatus       = EMS_TX_REV_DETECT;
    EMS_Sys_Status.emsRefreshedFlags = EMS_DEVICE_UPDATE_FLAG_NONE;
    EMS_Sys_Status.emsPollEnabled    = false; // start up with Poll disabled
    EMS_Sys_Status.emsBusConnected   = false;
    EMS_Sys_Status.emsRxTimestamp    = 0;
    EMS_Sys_Status.emsTxCapable      = false;
    EMS_Sys_Status.emsTxDisabled     = false;
    EMS_Sys_Status.emsPollFrequency  = 0;
    EMS_Sys_Status.txRetryCount      = 0;
    EMS_Sys_Status.emsIDMask         = 0x00;
    EMS_Sys_Status.emsPollAck[0]     = EMS_ID_ME;

    // thermostat
    strlcpy(EMS_Thermostat.datetime, "?", sizeof(EMS_Thermostat.datetime));
    EMS_Thermostat.write_supported = false;
    EMS_Thermostat.device_id       = EMS_ID_NONE;

    // init all heating circuits
    for (uint8_t i = 0; i < EMS_THERMOSTAT_MAXHC; i++) {
        EMS_Thermostat.hc[i].hc                = i + 1;
        EMS_Thermostat.hc[i].active            = false;
        EMS_Thermostat.hc[i].mode              = EMS_VALUE_INT_NOTSET;
        EMS_Thermostat.hc[i].day_mode          = EMS_VALUE_INT_NOTSET;
        EMS_Thermostat.hc[i].summer_mode       = EMS_VALUE_INT_NOTSET;
        EMS_Thermostat.hc[i].holiday_mode      = EMS_VALUE_INT_NOTSET;
        EMS_Thermostat.hc[i].daytemp           = EMS_VALUE_INT_NOTSET;
        EMS_Thermostat.hc[i].nighttemp         = EMS_VALUE_INT_NOTSET;
        EMS_Thermostat.hc[i].holidaytemp       = EMS_VALUE_INT_NOTSET;
        EMS_Thermostat.hc[i].heatingtype       = EMS_VALUE_INT_NOTSET; // floor heating = 3
        EMS_Thermostat.hc[i].circuitcalctemp   = EMS_VALUE_INT_NOTSET;
        EMS_Thermostat.hc[i].setpoint_roomTemp = EMS_VALUE_SHORT_NOTSET;
        EMS_Thermostat.hc[i].curr_roomTemp     = EMS_VALUE_SHORT_NOTSET;
    }

    EMS_Mixing.detected = false;
    // init all mixing modules
    for (uint8_t i = 0; i < EMS_THERMOSTAT_MAXHC; i++) {
        EMS_Mixing.hc[i].hc          = i + 1;
        EMS_Mixing.hc[i].flowTemp    = EMS_VALUE_SHORT_NOTSET;
        EMS_Mixing.hc[i].pumpMod     = EMS_VALUE_INT_NOTSET;
        EMS_Mixing.hc[i].valveStatus = EMS_VALUE_INT_NOTSET;
    }

    // UBAParameterWW
    EMS_Boiler.wWActivated   = EMS_VALUE_BOOL_NOTSET; // Warm Water activated
    EMS_Boiler.wWSelTemp     = EMS_VALUE_INT_NOTSET;  // Warm Water selected temperature
    EMS_Boiler.wWCircPump    = EMS_VALUE_BOOL_NOTSET; // Warm Water circulation pump available
    EMS_Boiler.wWDesiredTemp = EMS_VALUE_INT_NOTSET;  // Warm Water desired temperature to prevent infection
    EMS_Boiler.wWComfort     = EMS_VALUE_INT_NOTSET;  // WW comfort mode

    // UBAMonitorFast
    EMS_Boiler.selFlowTemp = EMS_VALUE_INT_NOTSET;    // Selected flow temperature
    EMS_Boiler.curFlowTemp = EMS_VALUE_USHORT_NOTSET; // Current flow temperature
    EMS_Boiler.retTemp     = EMS_VALUE_USHORT_NOTSET; // Return temperature
    EMS_Boiler.burnGas     = EMS_VALUE_BOOL_NOTSET;   // Gas on/off
    EMS_Boiler.fanWork     = EMS_VALUE_BOOL_NOTSET;   // Fan on/off
    EMS_Boiler.ignWork     = EMS_VALUE_BOOL_NOTSET;   // Ignition on/off
    EMS_Boiler.heatPmp     = EMS_VALUE_BOOL_NOTSET;   // Boiler pump on/off
    EMS_Boiler.wWHeat      = EMS_VALUE_INT_NOTSET;    // 3-way valve on WW
    EMS_Boiler.wWCirc      = EMS_VALUE_BOOL_NOTSET;   // Circulation on/off
    EMS_Boiler.selBurnPow  = EMS_VALUE_INT_NOTSET;    // Burner max power %
    EMS_Boiler.curBurnPow  = EMS_VALUE_INT_NOTSET;    // Burner current power %
    EMS_Boiler.flameCurr   = EMS_VALUE_USHORT_NOTSET; // Flame current in micro amps
    EMS_Boiler.sysPress    = EMS_VALUE_INT_NOTSET;    // System pressure
    strlcpy(EMS_Boiler.serviceCodeChar, "??", sizeof(EMS_Boiler.serviceCodeChar));
    EMS_Boiler.serviceCode = EMS_VALUE_USHORT_NOTSET;

    // UBAMonitorSlow
    EMS_Boiler.extTemp     = EMS_VALUE_SHORT_NOTSET;  // Outside temperature
    EMS_Boiler.boilTemp    = EMS_VALUE_USHORT_NOTSET; // Boiler temperature
    EMS_Boiler.pumpMod     = EMS_VALUE_INT_NOTSET;    // Pump modulation %
    EMS_Boiler.burnStarts  = EMS_VALUE_LONG_NOTSET;   // # burner restarts
    EMS_Boiler.burnWorkMin = EMS_VALUE_LONG_NOTSET;   // Total burner operating time
    EMS_Boiler.heatWorkMin = EMS_VALUE_LONG_NOTSET;   // Total heat operating time

    // UBAMonitorWWMessage
    EMS_Boiler.wWCurTmp  = EMS_VALUE_USHORT_NOTSET; // Warm Water current temperature
    EMS_Boiler.wWStarts  = EMS_VALUE_LONG_NOTSET;   // Warm Water # starts
    EMS_Boiler.wWWorkM   = EMS_VALUE_LONG_NOTSET;   // Warm Water # minutes
    EMS_Boiler.wWOneTime = EMS_VALUE_INT_NOTSET;    // Warm Water one time function on/off
    EMS_Boiler.wWCurFlow = EMS_VALUE_INT_NOTSET;    // WW current flow temp

    // UBATotalUptimeMessage
    EMS_Boiler.UBAuptime = EMS_VALUE_LONG_NOTSET; // Total UBA working hours

    // UBAParametersMessage
    EMS_Boiler.heating_temp = EMS_VALUE_INT_NOTSET; // Heating temperature setting on the boiler
    EMS_Boiler.pump_mod_max = EMS_VALUE_INT_NOTSET; // Boiler circuit pump modulation max. power %
    EMS_Boiler.pump_mod_min = EMS_VALUE_INT_NOTSET; // Boiler circuit pump modulation min. power %

    // Solar Module values
    EMS_SolarModule.collectorTemp          = EMS_VALUE_SHORT_NOTSET; // collector temp from SM10/SM100
    EMS_SolarModule.bottomTemp             = EMS_VALUE_SHORT_NOTSET; // bottom temp from SM10/SM100
    EMS_SolarModule.pumpModulation         = EMS_VALUE_INT_NOTSET;   // modulation solar pump SM10/SM100
    EMS_SolarModule.pump                   = EMS_VALUE_BOOL_NOTSET;  // pump active
    EMS_SolarModule.EnergyLastHour         = EMS_VALUE_USHORT_NOTSET;
    EMS_SolarModule.EnergyToday            = EMS_VALUE_USHORT_NOTSET;
    EMS_SolarModule.EnergyTotal            = EMS_VALUE_USHORT_NOTSET;
    EMS_SolarModule.device_id              = EMS_ID_NONE;
    EMS_SolarModule.product_id             = EMS_ID_NONE;
    EMS_SolarModule.pumpWorkMin            = EMS_VALUE_LONG_NOTSET;
    EMS_SolarModule.setpoint_maxBottomTemp = EMS_VALUE_SHORT_NOTSET;

    // Other EMS devices values
    EMS_HeatPump.HPModulation = EMS_VALUE_INT_NOTSET;
    EMS_HeatPump.HPSpeed      = EMS_VALUE_INT_NOTSET;
    EMS_HeatPump.device_id    = EMS_ID_NONE;
    EMS_HeatPump.product_id   = EMS_ID_NONE;

    // calculated values
    EMS_Boiler.tapwaterActive = EMS_VALUE_BOOL_NOTSET; // Hot tap water is on/off
    EMS_Boiler.heatingActive  = EMS_VALUE_BOOL_NOTSET; // Central heating is on/off

    // set boiler type
    EMS_Boiler.product_id = EMS_ID_NONE;
    strlcpy(EMS_Boiler.version, "?", sizeof(EMS_Boiler.version));

    // set thermostat model
    EMS_Thermostat.product_id = EMS_ID_NONE;
    strlcpy(EMS_Thermostat.version, "?", sizeof(EMS_Thermostat.version));

    // default logging is none
    ems_setLogging(EMS_SYS_LOGGING_DEFAULT);
}

// Getters and Setters for parameters
void ems_setPoll(bool b) {
    EMS_Sys_Status.emsPollEnabled = b;
    myDebug_P(PSTR("EMS Bus Poll is set to %s"), EMS_Sys_Status.emsPollEnabled ? "enabled" : "disabled");
}

bool ems_getPoll() {
    return EMS_Sys_Status.emsPollEnabled;
}

bool ems_getBoilerEnabled() {
    return (EMS_Boiler.device_id != EMS_ID_NONE);
}

bool ems_getThermostatEnabled() {
    return (EMS_Thermostat.device_id != EMS_ID_NONE);
}

bool ems_getMixingDeviceEnabled() {
    return EMS_Mixing.detected;
}

bool ems_getSolarModuleEnabled() {
    return (EMS_SolarModule.device_id != EMS_ID_NONE);
}

bool ems_getHeatPumpEnabled() {
    return (EMS_HeatPump.device_id != EMS_ID_NONE);
}

uint8_t ems_getThermostatModel() {
    return (EMS_Thermostat.device_flags & 0x7F); // strip 7th bit
}

uint8_t ems_getSolarModuleModel() {
    return (EMS_SolarModule.device_flags);
}

void ems_setTxDisabled(bool b) {
    EMS_Sys_Status.emsTxDisabled = b;
}

bool ems_getTxDisabled() {
    return (EMS_Sys_Status.emsTxDisabled);
}

uint32_t ems_getPollFrequency() {
    return EMS_Sys_Status.emsPollFrequency;
}

bool ems_getTxCapable() {
    if ((EMS_Sys_Status.emsPollFrequency == 0) || (EMS_Sys_Status.emsPollFrequency > EMS_POLL_TIMEOUT)) {
        EMS_Sys_Status.emsTxCapable = false;
    }
    return EMS_Sys_Status.emsTxCapable;
}

bool ems_getBusConnected() {
    if ((millis() - EMS_Sys_Status.emsRxTimestamp) > EMS_BUS_TIMEOUT) {
        EMS_Sys_Status.emsBusConnected = false;
    }
    return EMS_Sys_Status.emsBusConnected;
}

_EMS_SYS_LOGGING ems_getLogging() {
    return EMS_Sys_Status.emsLogging;
}

void ems_setLogging(_EMS_SYS_LOGGING loglevel, uint16_t type_id) {
    if (loglevel <= EMS_SYS_LOGGING_JABBER) {
        EMS_Sys_Status.emsLogging = loglevel;

        if (loglevel == EMS_SYS_LOGGING_NONE) {
            myDebug_P(PSTR("System Logging set to None"));
        } else if (loglevel == EMS_SYS_LOGGING_BASIC) {
            myDebug_P(PSTR("System Logging set to Basic"));
        } else if (loglevel == EMS_SYS_LOGGING_VERBOSE) {
            myDebug_P(PSTR("System Logging set to Verbose"));
        } else if (loglevel == EMS_SYS_LOGGING_THERMOSTAT) {
            myDebug_P(PSTR("System Logging set to Thermostat only"));
        } else if (loglevel == EMS_SYS_LOGGING_SOLARMODULE) {
            myDebug_P(PSTR("System Logging set to Solar Module only"));
        } else if (loglevel == EMS_SYS_LOGGING_RAW) {
            myDebug_P(PSTR("System Logging set to Raw mode"));
        } else if (loglevel == EMS_SYS_LOGGING_JABBER) {
            myDebug_P(PSTR("System Logging set to Jabber mode"));
        } else if (loglevel == EMS_SYS_LOGGING_WATCH) {
            EMS_Sys_Status.emsLogging_typeID = type_id;
            myDebug_P(PSTR("System Logging set to Watch mode"));
        }
    }
}

/**
 * send a poll acknowledge
 */
void ems_tx_pollAck() {
    emsuart_tx_buffer(&EMS_Sys_Status.emsPollAck[0], 1);
}

/**
 * Calculate CRC checksum using lookup table for speed
 * len is length of all the data in bytes (including the header & CRC byte at end)
 */
uint8_t _crcCalculator(uint8_t * data, uint8_t len) {
    if (len <= 1) {
        return 0;
    }

    uint8_t crc = 0;

    // read data and stop before the CRC
    for (uint8_t i = 0; i < len - 1; i++) {
        crc = ems_crc_table[crc];
        crc ^= data[i];
    }

    return crc;
}

// unsigned short
void _setValue(_EMS_RxTelegram * EMS_RxTelegram, uint16_t * param_op, uint8_t index) {
    if (index >= EMS_RxTelegram->data_length) {
        return;
    }

    uint16_t value = (EMS_RxTelegram->data[index] << 8) + EMS_RxTelegram->data[index + 1];

    // check for undefined/unset values, 0x8000
    if (value == EMS_VALUE_USHORT_NOTSET) {
        return;
    }

    *param_op = value;
}

// signed short
void _setValue(_EMS_RxTelegram * EMS_RxTelegram, int16_t * param_op, uint8_t index) {
    if (index >= EMS_RxTelegram->data_length) {
        return;
    }

    int16_t value = (EMS_RxTelegram->data[index] << 8) + EMS_RxTelegram->data[index + 1];

    // check for undefined/unset values, 0x8000
    if ((value == EMS_VALUE_SHORT_NOTSET) || (EMS_RxTelegram->data[index] == 0x7D)) {
        return;
    }

    *param_op = value;
}

// Byte
void _setValue(_EMS_RxTelegram * EMS_RxTelegram, uint8_t * param_op, uint8_t index) {
    if (index >= EMS_RxTelegram->data_length) {
        return;
    }

    *param_op = (uint8_t)EMS_RxTelegram->data[index];
}

// convert signed short to single 8 byte, for setpoint thermostat temperatures that don't store their temps in 2 bytes
void _setValue8(_EMS_RxTelegram * EMS_RxTelegram, int16_t * param_op, uint8_t index) {
    if (index >= EMS_RxTelegram->data_length) {
        return;
    }

    *param_op = EMS_RxTelegram->data[index];
}

// Long
void _setValue(_EMS_RxTelegram * EMS_RxTelegram, uint32_t * param_op, uint8_t index) {
    if (index >= EMS_RxTelegram->data_length) {
        return;
    }

    *param_op = (uint32_t)((EMS_RxTelegram->data[index] << 16) + (EMS_RxTelegram->data[index + 1] << 8) + (EMS_RxTelegram->data[index + 2]));
}

// bit from a byte
void _setValue(_EMS_RxTelegram * EMS_RxTelegram, uint8_t * param_op, uint8_t index, uint8_t bit) {
    if (index >= EMS_RxTelegram->data_length) {
        return;
    }

    *param_op = (uint8_t)(((EMS_RxTelegram->data[index]) >> (bit)) & 0x01);
}

void ems_setTxMode(uint8_t mode) {
    EMS_Sys_Status.emsTxMode = mode;
}

/**
 * debug print a telegram to telnet/serial including the CRC
 */
void _debugPrintTelegram(const char * prefix, _EMS_RxTelegram * EMS_RxTelegram, const char * color, bool raw = false) {
    char      output_str[200] = {0};
    char      buffer[16]      = {0};
    uint8_t * data            = EMS_RxTelegram->telegram;
    uint8_t   data_len        = EMS_RxTelegram->data_length; // length of data block
    uint8_t   length          = EMS_RxTelegram->length;      // includes CRC

    // get elapsed system time or internet time if available
    uint8_t       t_sec, t_min, t_hour;
    uint16_t      t_msec;
    unsigned long timestamp   = EMS_RxTelegram->timestamp;
    bool          haveNTPtime = (timestamp > 1572307205); // after Jan 1st 1970

    if (haveNTPtime) {
        t_sec = timestamp % 60;
        timestamp /= 60; // now it is minutes
        t_min = timestamp % 60;
        timestamp /= 60; // now it is hours
        t_hour = timestamp % 24;
    } else {
        t_hour = timestamp / 3600000;
        t_min  = (timestamp / 60000) % 60;
        t_sec  = (timestamp / 1000) % 60;
        t_msec = timestamp % 1000;
    }

    strlcpy(output_str, "(", sizeof(output_str));

    if (!raw)
        strlcat(output_str, COLOR_CYAN, sizeof(output_str));

    strlcat(output_str, _smallitoa(t_hour, buffer), sizeof(output_str));
    strlcat(output_str, ":", sizeof(output_str));
    strlcat(output_str, _smallitoa(t_min, buffer), sizeof(output_str));
    strlcat(output_str, ":", sizeof(output_str));
    strlcat(output_str, _smallitoa(t_sec, buffer), sizeof(output_str));

    // internet time doesn't have millisecond precision, so ignore it
    if (!haveNTPtime) {
        strlcat(output_str, ".", sizeof(output_str));
        strlcat(output_str, _smallitoa3(t_msec, buffer), sizeof(output_str));
    }

    if (!raw)
        strlcat(output_str, COLOR_RESET, sizeof(output_str));

    strlcat(output_str, ") ", sizeof(output_str));

    if (!raw)
        strlcat(output_str, color, sizeof(output_str));

    strlcat(output_str, prefix, sizeof(output_str));

    if (!raw) {
        strlcat(output_str, "telegram: ", sizeof(output_str));
    }

    for (int i = 0; i < (length - 1); i++) {
        strlcat(output_str, _hextoa(data[i], buffer), sizeof(output_str));
        strlcat(output_str, " ", sizeof(output_str)); // add space
    }

    if (!raw) {
        strlcat(output_str, "(CRC=", sizeof(output_str));
        strlcat(output_str, _hextoa(data[length - 1], buffer), sizeof(output_str));
        strlcat(output_str, ")", sizeof(output_str));

        // print number of data bytes only if its a valid telegram
        if (data_len) {
            strlcat(output_str, " #data=", sizeof(output_str));
            strlcat(output_str, itoa(data_len, buffer, 10), sizeof(output_str));
        }

        strlcat(output_str, COLOR_RESET, sizeof(output_str));
    } else {
        // send it the SysLog
        myESP.writeLogEvent(MYESP_SYSLOG_INFO, output_str);
    }

    myDebug(output_str);
}

/**
 * send the contents of the Tx buffer to the UART
 * we take telegram from the queue and send it, but don't remove it until later when its confirmed successful
 */
void _ems_sendTelegram() {
    // check if we have something in the queue to send
    if (EMS_TxQueue.isEmpty()) {
        return;
    }

    // if we're preventing all outbound traffic, quit
    if (ems_getTxDisabled()) {
        EMS_TxQueue.shift(); // remove from queue
        return;
    }

    // get the first in the queue, which is at the head
    // we don't remove from the queue yet
    _EMS_TxTelegram EMS_TxTelegram = EMS_TxQueue.first();

    // if we're in raw mode just fire and forget
    if (EMS_TxTelegram.action == EMS_TX_TELEGRAM_RAW) {
        EMS_TxTelegram.data[EMS_TxTelegram.length - 1] = _crcCalculator(EMS_TxTelegram.data, EMS_TxTelegram.length); // add the CRC

        if (EMS_Sys_Status.emsLogging != EMS_SYS_LOGGING_NONE) {
            _EMS_RxTelegram EMS_RxTelegram;                     // create new Rx object
            EMS_RxTelegram.length      = EMS_TxTelegram.length; // full length of telegram
            EMS_RxTelegram.telegram    = EMS_TxTelegram.data;
            EMS_RxTelegram.data_length = 0;                     // ignore #data=
            EMS_RxTelegram.timestamp   = myESP.getSystemTime(); // now
            _debugPrintTelegram("Sending raw: ", &EMS_RxTelegram, COLOR_CYAN, true);
        }

        _EMS_TX_STATUS _txStatus = emsuart_tx_buffer(EMS_TxTelegram.data, EMS_TxTelegram.length); // send the telegram to the UART Tx
        if (EMS_TX_BRK_DETECT == _txStatus || EMS_TX_WTD_TIMEOUT == _txStatus) {
            // Tx Error!
            if (EMS_Sys_Status.emsLogging == EMS_SYS_LOGGING_VERBOSE) {
                myDebug_P(PSTR("** error sending buffer: %s"), _txStatus == EMS_TX_BRK_DETECT ? "BRK" : "WDTO");
            }
            // EMS_Sys_Status.emsTxStatus = EMS_TX_STATUS_IDLE;
        }
        EMS_TxQueue.shift(); // and remove from queue
        return;
    }

    // create the header
    EMS_TxTelegram.data[0] = EMS_ID_ME ^ EMS_Sys_Status.emsIDMask; // src

    // dest
    if (EMS_TxTelegram.action == EMS_TX_TELEGRAM_WRITE) {
        EMS_TxTelegram.data[1] = EMS_TxTelegram.dest;
    } else {
        // for a READ or VALIDATE
        EMS_TxTelegram.data[1] = (EMS_TxTelegram.dest | 0x80); // read has 8th bit set, always
    }

    // complete the rest of the header depending on EMS or EMS+
    if (EMS_TxTelegram.type > 0xFF) {
        // EMS 2.0 / EMS+
        EMS_TxTelegram.data[2] = 0xFF; // fixed value indicating an extended message
        EMS_TxTelegram.data[3] = EMS_TxTelegram.offset;
        EMS_TxTelegram.length += 2; // add 2 bytes to length to compensate the extra FF and byte for the type

        // EMS+ has different format for read and write. See https://github.com/proddy/EMS-ESP/wiki/RC3xx-Thermostats
        if ((EMS_TxTelegram.action == EMS_TX_TELEGRAM_READ) || (EMS_TxTelegram.action == EMS_TX_TELEGRAM_VALIDATE)) {
            EMS_TxTelegram.data[4] = EMS_TxTelegram.dataValue;   // for read its #bytes to return
            EMS_TxTelegram.data[5] = EMS_TxTelegram.type >> 8;   // type, 1st byte
            EMS_TxTelegram.data[6] = EMS_TxTelegram.type & 0xFF; // type, 2nd byte
        } else if (EMS_TxTelegram.action == EMS_TX_TELEGRAM_WRITE) {
            EMS_TxTelegram.data[4] = EMS_TxTelegram.type >> 8;   // type, 1st byte
            EMS_TxTelegram.data[5] = EMS_TxTelegram.type & 0xFF; // type, 2nd byte
            EMS_TxTelegram.data[6] = EMS_TxTelegram.dataValue;   // for write it the value to set
        }
    } else {
        // EMS 1.0
        EMS_TxTelegram.data[2] = EMS_TxTelegram.type;   // type
        EMS_TxTelegram.data[3] = EMS_TxTelegram.offset; // offset
        if (EMS_TxTelegram.length == EMS_MIN_TELEGRAM_LENGTH) {
            EMS_TxTelegram.data[4] = EMS_TxTelegram.dataValue; // for read its #bytes to return, for write it the value to set
        }
    }

    // finally calculate CRC and add it to the end
    EMS_TxTelegram.data[EMS_TxTelegram.length - 1] = _crcCalculator(EMS_TxTelegram.data, EMS_TxTelegram.length);

    // print debug info
    if (EMS_Sys_Status.emsLogging == EMS_SYS_LOGGING_VERBOSE) {
        char s[64] = {0};
        if (EMS_TxTelegram.action == EMS_TX_TELEGRAM_WRITE) {
            snprintf(s, sizeof(s), "Sending write of type 0x%02X to 0x%02X, ", EMS_TxTelegram.type, EMS_TxTelegram.dest & 0x7F);
        } else if (EMS_TxTelegram.action == EMS_TX_TELEGRAM_READ) {
            snprintf(s, sizeof(s), "Sending read of type 0x%02X to 0x%02X, ", EMS_TxTelegram.type, EMS_TxTelegram.dest & 0x7F);
        } else if (EMS_TxTelegram.action == EMS_TX_TELEGRAM_VALIDATE) {
            snprintf(s, sizeof(s), "Sending validate of type 0x%02X to 0x%02X, ", EMS_TxTelegram.type, EMS_TxTelegram.dest & 0x7F);
        }

        _EMS_RxTelegram EMS_RxTelegram;
        EMS_RxTelegram.length      = EMS_TxTelegram.length; // complete length of telegram incl CRC
        EMS_RxTelegram.data_length = 0;                     // ignore the data length for read and writes. only used for incoming.
        EMS_RxTelegram.telegram    = EMS_TxTelegram.data;
        EMS_RxTelegram.timestamp   = myESP.getSystemTime(); // now
        _debugPrintTelegram(s, &EMS_RxTelegram, COLOR_CYAN);
    }

    // send the telegram to the UART Tx
    _EMS_TX_STATUS _txStatus = emsuart_tx_buffer(EMS_TxTelegram.data, EMS_TxTelegram.length); // send the telegram to the UART Tx
    if (EMS_TX_STATUS_OK == _txStatus || EMS_TX_STATUS_IDLE == _txStatus)
        EMS_Sys_Status.emsTxStatus = EMS_TX_STATUS_WAIT;
    else {
        // Tx Error!
        if (EMS_Sys_Status.emsLogging == EMS_SYS_LOGGING_VERBOSE) {
            myDebug_P(PSTR("** error sending buffer: %s"), _txStatus == EMS_TX_BRK_DETECT ? "BRK" : "WDTO");
        }
        EMS_Sys_Status.emsTxStatus = EMS_TX_STATUS_IDLE;
    }
}

/**
 * Takes the last write command and turns into a validate request
 * placing it on the Tx queue
 */
void _createValidate() {
    if (EMS_TxQueue.isEmpty()) {
        return;
    }

    // release the Tx lock
    EMS_Sys_Status.emsTxStatus = EMS_TX_STATUS_IDLE;

    // get the first in the queue, which is at the head
    _EMS_TxTelegram EMS_TxTelegram = EMS_TxQueue.first();

    // safety check: only do a validate after a write and when we have a type to validate
    if ((EMS_TxTelegram.action != EMS_TX_TELEGRAM_WRITE) || (EMS_TxTelegram.type_validate == EMS_ID_NONE)) {
        EMS_TxQueue.shift(); // remove from queue
        return;
    }

    // create a new Telegram copying from the last write
    _EMS_TxTelegram new_EMS_TxTelegram;
    new_EMS_TxTelegram.action = EMS_TX_TELEGRAM_VALIDATE;

    // copy old Write record
    new_EMS_TxTelegram.type_validate      = EMS_TxTelegram.type;          // save the original type in the type_validate, increase we need to re-try
    new_EMS_TxTelegram.type               = EMS_TxTelegram.type_validate; // new type is the validate type
    new_EMS_TxTelegram.dest               = EMS_TxTelegram.dest;
    new_EMS_TxTelegram.comparisonValue    = EMS_TxTelegram.comparisonValue;
    new_EMS_TxTelegram.comparisonPostRead = EMS_TxTelegram.comparisonPostRead;
    new_EMS_TxTelegram.comparisonOffset   = EMS_TxTelegram.comparisonOffset;

    // this is what is different
    new_EMS_TxTelegram.offset    = EMS_TxTelegram.comparisonOffset; // location of byte to fetch
    new_EMS_TxTelegram.dataValue = 1;                               // fetch one byte
    new_EMS_TxTelegram.length    = EMS_MIN_TELEGRAM_LENGTH;         // is always 6 bytes long (including CRC at end)
    new_EMS_TxTelegram.timestamp = millis();

    // remove old telegram from queue and add this new read one
    EMS_TxQueue.shift();                     // remove from queue
    EMS_TxQueue.unshift(new_EMS_TxTelegram); // add back to queue making it first to be picked up next (FIFO)
}

/**
 * dump a UART Tx or Rx buffer to console...
 */
void ems_dumpBuffer(const char * prefix, uint8_t * telegram, uint8_t length) {
    uint32_t    timestamp       = millis();
    static char output_str[200] = {0};
    static char buffer[16]      = {0};

    strlcpy(output_str, "(", sizeof(output_str));
    strlcat(output_str, COLOR_CYAN, sizeof(output_str));
    strlcat(output_str, _smallitoa((uint8_t)((timestamp / 3600000) % 24), buffer), sizeof(output_str));
    strlcat(output_str, ":", sizeof(output_str));
    strlcat(output_str, _smallitoa((uint8_t)((timestamp / 60000) % 60), buffer), sizeof(output_str));
    strlcat(output_str, ":", sizeof(output_str));
    strlcat(output_str, _smallitoa((uint8_t)((timestamp / 1000) % 60), buffer), sizeof(output_str));
    strlcat(output_str, ".", sizeof(output_str));
    strlcat(output_str, _smallitoa3(timestamp % 1000, buffer), sizeof(output_str));
    strlcat(output_str, COLOR_RESET, sizeof(output_str));
    strlcat(output_str, ") ", sizeof(output_str));

    strlcat(output_str, COLOR_YELLOW, sizeof(output_str));
    strlcat(output_str, prefix, sizeof(output_str));

    // show some EMS_Sys_Status entries
    strlcat(output_str, _hextoa(EMS_Sys_Status.emsRxStatus, buffer), sizeof(output_str));
    strlcat(output_str, " ", sizeof(output_str));
    strlcat(output_str, _hextoa(EMS_Sys_Status.emsTxStatus, buffer), sizeof(output_str));
    strlcat(output_str, ": ", sizeof(output_str));

    // print whole buffer, don't interpret any data
    for (int i = 0; i < (length); i++) {
        strlcat(output_str, _hextoa(telegram[i], buffer), sizeof(output_str));
        strlcat(output_str, " ", sizeof(output_str));
    }

    strlcat(output_str, COLOR_RESET, sizeof(output_str));

    myDebug(output_str);
}

/**
 * Entry point triggered by an interrupt in emsuart.cpp
 * length is the number of all the telegram bytes up to and including the CRC at the end
 * Read commands are asynchronous as they're handled by the interrupt
 * When a telegram is processed we forcefully erase it from the stack to prevent overflow
 */
void ems_parseTelegram(uint8_t * telegram, uint8_t length) {
    if (EMS_Sys_Status.emsLogging == EMS_SYS_LOGGING_JABBER) {
        ems_dumpBuffer("ems_parseTelegram: ", telegram, length);
    }

    /*
     * Detect the EMS bus type - Buderus or Junkers - and set emsIDMask accordingly.
     *  we wait for the first valid telegram and look at the SourceID.
     *  If Bit 7 is set we have a Buderus, otherwise a Junkers
     */
    if (EMS_Sys_Status.emsTxStatus == EMS_TX_REV_DETECT) {
        if ((length >= 5) && (telegram[length - 1] == _crcCalculator(telegram, length))) {
            EMS_Sys_Status.emsTxStatus   = EMS_TX_STATUS_IDLE;
            EMS_Sys_Status.emsIDMask     = telegram[0] & 0x80;
            EMS_Sys_Status.emsPollAck[0] = EMS_ID_ME ^ EMS_Sys_Status.emsIDMask;
        } else
            return; // ignore the whole telegram Rx Telegram while in DETECT mode
    }

    /* 
     * It may happen that we were interrupted (for instance by WIFI activity) and the 
     * buffer isn't valid anymore, so we must not answer at all...
     */
    if (EMS_Sys_Status.emsRxStatus != EMS_RX_STATUS_IDLE) {
        if (EMS_Sys_Status.emsLogging > EMS_SYS_LOGGING_NONE) {
            myDebug_P(PSTR("** Warning, we missed the bus - Rx non-idle!"));
        }
        return;
    }

    /*
     * check if we just received one byte
     * it could well be a Poll request from the boiler for us, which will have a value of 0x8B (0x0B | 0x80)
     * or either a return code like 0x01 or 0x04 from the last Write command
     */
    if (length == 1) {
        uint8_t         value                  = telegram[0]; // 1st byte of data package
        static uint32_t _last_emsPollFrequency = 0;

        // check first for a Poll for us
        if ((value ^ 0x80 ^ EMS_Sys_Status.emsIDMask) == EMS_ID_ME) {
            uint32_t timenow_microsecs      = micros();
            EMS_Sys_Status.emsPollFrequency = (timenow_microsecs - _last_emsPollFrequency);
            _last_emsPollFrequency          = timenow_microsecs;

            // do we have something to send thats waiting in the Tx queue?
            // if so send it if the Queue is not in a wait state
            if ((!EMS_TxQueue.isEmpty()) && (EMS_Sys_Status.emsTxStatus == EMS_TX_STATUS_IDLE)) {
                _ems_sendTelegram(); // perform the read/write command immediately
            } else {
                // nothing to send so just send a poll acknowledgement back
                if (EMS_Sys_Status.emsPollEnabled) {
                    ems_tx_pollAck();
                }
            }
        } else if (EMS_Sys_Status.emsTxStatus == EMS_TX_STATUS_WAIT) {
            // this may be a byte 01 (success) or 04 (error) from a recent write command?
            if (value == EMS_TX_SUCCESS) {
                EMS_Sys_Status.emsTxPkgs++;
                // got a success 01. Send a validate to check the value of the last write
                ems_tx_pollAck();  // send a poll to free the EMS bus
                _createValidate(); // create a validate Tx request (if needed)
            } else if (value == EMS_TX_ERROR) {
                // last write failed (04), delete it from queue and dont bother to retry
                if (EMS_Sys_Status.emsLogging == EMS_SYS_LOGGING_VERBOSE) {
                    myDebug_P(PSTR("-> Error: Write command failed from host"));
                }
                ems_tx_pollAck(); // send a poll to free the EMS bus
                _removeTxQueue(); // remove from queue
            }
        }

        return; // all done here
    }

    // ignore anything that doesn't resemble a proper telegram package
    // minimal is 5 bytes, excluding CRC at the end (for EMS1.0)
    if (length <= 4) {
        // _debugPrintTelegram("Noisy data: ", &EMS_RxTelegram, COLOR_RED);
        return;
    }

    static _EMS_RxTelegram EMS_RxTelegram; // create the Rx package
    EMS_RxTelegram.telegram  = telegram;
    EMS_RxTelegram.timestamp = myESP.getSystemTime();
    EMS_RxTelegram.length    = length;

    EMS_RxTelegram.src    = telegram[0] & 0x7F; // removing 8th bit as we deal with both reads and writes here
    EMS_RxTelegram.dest   = telegram[1] & 0x7F; // remove 8th bit (don't care if read or write)
    EMS_RxTelegram.offset = telegram[3];        // offset is always 4th byte

    // determing if its normal ems or ems plus, check for marker
    if (telegram[2] >= 0xF0) {
        // its EMS plus / EMS 2.0
        EMS_RxTelegram.emsplus      = true;
        EMS_RxTelegram.emsplus_type = telegram[2]; // 0xFF, 0xF7 or 0xF9

        if (EMS_RxTelegram.emsplus_type == 0xFF) {
            EMS_RxTelegram.type = (telegram[4] << 8) + telegram[5]; // is a long in bytes 5 & 6
            EMS_RxTelegram.data = telegram + 6;

            if (length <= 7) {
                EMS_RxTelegram.data_length = 0; // special broadcast on ems+ have no data values
            } else {
                EMS_RxTelegram.data_length = length - 7; // remove 6 byte header plus CRC
            }
        } else {
            // its F9 or F7
            uint8_t shift       = (telegram[4] != 0xFF); // true (1) if byte 4 is not 0xFF, then telegram is 1 byte longer
            EMS_RxTelegram.type = (telegram[5 + shift] << 8) + telegram[6 + shift];
            EMS_RxTelegram.data = telegram + 6 + shift; // there is a special byte after the typeID which we ignore for now
            if (length <= (9 + shift)) {
                EMS_RxTelegram.data_length = 0; // special broadcast on ems+ have no data values
            } else {
                EMS_RxTelegram.data_length = length - (9 + shift);
            }
        }
    } else {
        // Normal EMS 1.0
        EMS_RxTelegram.emsplus     = false;
        EMS_RxTelegram.type        = telegram[2]; // 3rd byte
        EMS_RxTelegram.data        = telegram + 4;
        EMS_RxTelegram.data_length = length - 5; // remove 4 bytes header plus CRC
    }

    // if we are in raw logging mode then just print out the telegram as it is
    // else if we're watching a specific type ID show it and also log an event to the SysLog
    // but still continue to process it
    if ((EMS_Sys_Status.emsLogging == EMS_SYS_LOGGING_RAW)) {
        _debugPrintTelegram("", &EMS_RxTelegram, COLOR_WHITE, true);
    } else if ((EMS_Sys_Status.emsLogging == EMS_SYS_LOGGING_WATCH) && (EMS_RxTelegram.type == EMS_Sys_Status.emsLogging_typeID)) {
        _debugPrintTelegram("", &EMS_RxTelegram, COLOR_WHITE, true);
    }

    // Assume at this point we have something that vaguely resembles a telegram in the format [src] [dest] [type] [offset] [data] [crc]
    // validate the CRC, if it's bad ignore it
    if (telegram[length - 1] != _crcCalculator(telegram, length)) {
        EMS_Sys_Status.emxCrcErr++;
        if (EMS_Sys_Status.emsLogging == EMS_SYS_LOGGING_VERBOSE) {
            _debugPrintTelegram("Corrupt telegram: ", &EMS_RxTelegram, COLOR_RED, true);
        }
        return;
    }

    // here we know its a valid incoming telegram of at least 6 bytes
    // we use this to see if we always have a connection to the boiler, in case of drop outs
    EMS_Sys_Status.emsRxTimestamp  = millis(); // timestamp of last read
    EMS_Sys_Status.emsBusConnected = true;

    // now lets process it and see what to do next
    _processType(&EMS_RxTelegram);
}

/**
 * print the telegram
 */
void _printMessage(_EMS_RxTelegram * EMS_RxTelegram) {
    // header info
    uint8_t  src    = EMS_RxTelegram->src;
    uint8_t  dest   = EMS_RxTelegram->dest;
    uint16_t type   = EMS_RxTelegram->type;
    uint8_t  length = EMS_RxTelegram->data_length;

    char output_str[200] = {0};
    char buffer[16]      = {0};
    char color_s[20]     = {0};
    char type_s[30];

    // source
    ems_getDeviceTypeDescription(src, type_s);
    strlcpy(output_str, type_s, sizeof(output_str));
    strlcat(output_str, " -> ", sizeof(output_str));

    // destination
    (void)ems_getDeviceTypeDescription(dest, type_s);
    strlcat(output_str, type_s, sizeof(output_str));

    if (dest == EMS_ID_ME) {
        strlcpy(color_s, COLOR_YELLOW, sizeof(color_s)); // me
    } else if (dest == EMS_ID_NONE) {
        strlcpy(color_s, COLOR_GREEN, sizeof(color_s)); // broadcast
    } else {
        strlcpy(color_s, COLOR_MAGENTA, sizeof(color_s)); // everything else
    }

    if (length) {
        // type
        strlcat(output_str, ", type 0x", sizeof(output_str));

        if (EMS_RxTelegram->emsplus) {
            strlcat(output_str, _hextoa(type >> 8, buffer), sizeof(output_str));
            strlcat(output_str, _hextoa(type & 0xFF, buffer), sizeof(output_str));
        } else {
            strlcat(output_str, _hextoa(type, buffer), sizeof(output_str));
        }
    }

    strlcat(output_str, ", ", sizeof(output_str));

    if (EMS_Sys_Status.emsLogging == EMS_SYS_LOGGING_THERMOSTAT) {
        // only print ones to/from thermostat if logging is set to thermostat only
        if ((src == EMS_Thermostat.device_id) || (dest == EMS_Thermostat.device_id)) {
            _debugPrintTelegram(output_str, EMS_RxTelegram, color_s);
        }
    } else if (EMS_Sys_Status.emsLogging == EMS_SYS_LOGGING_SOLARMODULE) {
        // only print ones to/from thermostat if logging is set to thermostat only
        if ((src == EMS_SolarModule.device_id) || (dest == EMS_SolarModule.device_id)) {
            _debugPrintTelegram(output_str, EMS_RxTelegram, color_s);
        }
    } else {
        // always print
        _debugPrintTelegram(output_str, EMS_RxTelegram, color_s);
    }
}

/**
 * Remove current Tx telegram from queue and release lock on Tx
 */
void _removeTxQueue() {
    if (!EMS_TxQueue.isEmpty()) {
        EMS_TxQueue.shift(); // remove item from top of the queue
    }
    EMS_Sys_Status.emsTxStatus = EMS_TX_STATUS_IDLE;
}

/**
 * Check if hot tap water or heating is active
 * using a quick hack for checking the heating. Selected Flow Temp >= 70
 */
void _checkActive() {
    // hot tap water, using flow to check instead of the burner power
    if (EMS_Boiler.wWCurFlow != EMS_VALUE_INT_NOTSET && EMS_Boiler.burnGas != EMS_VALUE_INT_NOTSET) {
        EMS_Boiler.tapwaterActive = ((EMS_Boiler.wWCurFlow != 0) && (EMS_Boiler.burnGas == EMS_VALUE_BOOL_ON));
    }

    // heating
    if (EMS_Boiler.selFlowTemp != EMS_VALUE_INT_NOTSET && EMS_Boiler.burnGas != EMS_VALUE_INT_NOTSET) {
        EMS_Boiler.heatingActive = ((EMS_Boiler.selFlowTemp >= EMS_BOILER_SELFLOWTEMP_HEATING) && (EMS_Boiler.burnGas == EMS_VALUE_BOOL_ON));
    }
}

/**
 * UBAParameterWW - type 0x33 - warm water parameters
 * received only after requested (not broadcasted)
 */
void _process_UBAParameterWW(_EMS_RxTelegram * EMS_RxTelegram) {
    _setValue(EMS_RxTelegram, &EMS_Boiler.wWActivated, 1); // 0xFF means on
    _setValue(EMS_RxTelegram, &EMS_Boiler.wWCircPump, 6);  // 0xFF means on
    _setValue(EMS_RxTelegram, &EMS_Boiler.wWSelTemp, 2);
    _setValue(EMS_RxTelegram, &EMS_Boiler.wWDesiredTemp, 8);
    _setValue(EMS_RxTelegram, &EMS_Boiler.wWComfort, EMS_OFFSET_UBAParameterWW_wwComfort);
}

/**
 * UBATotalUptimeMessage - type 0x14 - total uptime
 * received only after requested (not broadcasted)
 */
void _process_UBATotalUptimeMessage(_EMS_RxTelegram * EMS_RxTelegram) {
    _setValue(EMS_RxTelegram, &EMS_Boiler.UBAuptime, 0);
}

/**
 * UBAParametersMessage - type 0x16
 */
void _process_UBAParametersMessage(_EMS_RxTelegram * EMS_RxTelegram) {
    _setValue(EMS_RxTelegram, &EMS_Boiler.heating_temp, 1);
    _setValue(EMS_RxTelegram, &EMS_Boiler.pump_mod_max, 9);
    _setValue(EMS_RxTelegram, &EMS_Boiler.pump_mod_min, 10);
}

/**
 * UBAMonitorWWMessage - type 0x34 - warm water monitor. 19 bytes long
 * received every 10 seconds
 */
void _process_UBAMonitorWWMessage(_EMS_RxTelegram * EMS_RxTelegram) {
    _setValue(EMS_RxTelegram, &EMS_Boiler.wWCurTmp, 1);
    _setValue(EMS_RxTelegram, &EMS_Boiler.wWStarts, 13);
    _setValue(EMS_RxTelegram, &EMS_Boiler.wWWorkM, 10);
    _setValue(EMS_RxTelegram, &EMS_Boiler.wWOneTime, 5, 1);
    _setValue(EMS_RxTelegram, &EMS_Boiler.wWCurFlow, 9);
}

/**
 * Activate / De-activate One Time warm water 0x35
 * true = on, false = off
 */
void ems_setWarmWaterOnetime(bool activated) {
    myDebug_P(PSTR("Setting boiler warm water OneTime loading %s"), activated ? "on" : "off");

    _EMS_TxTelegram EMS_TxTelegram = EMS_TX_TELEGRAM_NEW; // create new Tx
    EMS_TxTelegram.timestamp       = millis();            // set timestamp
    EMS_Sys_Status.txRetryCount    = 0;                   // reset retry counter

    EMS_TxTelegram.action        = EMS_TX_TELEGRAM_WRITE;
    EMS_TxTelegram.dest          = EMS_Boiler.device_id;
    EMS_TxTelegram.type          = EMS_TYPE_UBAFlags;
    EMS_TxTelegram.offset        = EMS_OFFSET_UBAParameterWW_wwOneTime;
    EMS_TxTelegram.length        = EMS_MIN_TELEGRAM_LENGTH;
    EMS_TxTelegram.type_validate = EMS_ID_NONE;               // don't validate
    EMS_TxTelegram.dataValue     = (activated ? 0x22 : 0x02); // 0x22 is on, 0x02 is off for RC20RF

    EMS_TxQueue.push(EMS_TxTelegram);
}

/**
 * UBAMonitorFast - type 0x18 - central heating monitor part 1 (25 bytes long)
 * received every 10 seconds
 */
void _process_UBAMonitorFast(_EMS_RxTelegram * EMS_RxTelegram) {
    _setValue(EMS_RxTelegram, &EMS_Boiler.selFlowTemp, 0);
    _setValue(EMS_RxTelegram, &EMS_Boiler.curFlowTemp, 1);
    _setValue(EMS_RxTelegram, &EMS_Boiler.selBurnPow, 3); // burn power max setting
    _setValue(EMS_RxTelegram, &EMS_Boiler.curBurnPow, 4);

    _setValue(EMS_RxTelegram, &EMS_Boiler.burnGas, 7, 0);
    _setValue(EMS_RxTelegram, &EMS_Boiler.fanWork, 7, 2);
    _setValue(EMS_RxTelegram, &EMS_Boiler.ignWork, 7, 3);
    _setValue(EMS_RxTelegram, &EMS_Boiler.heatPmp, 7, 5);
    _setValue(EMS_RxTelegram, &EMS_Boiler.wWHeat, 7, 6);
    _setValue(EMS_RxTelegram, &EMS_Boiler.wWCirc, 7, 7);

    _setValue(EMS_RxTelegram, &EMS_Boiler.boilTemp, 11); // 0x8000 if not available
    _setValue(EMS_RxTelegram, &EMS_Boiler.retTemp, 13);
    _setValue(EMS_RxTelegram, &EMS_Boiler.flameCurr, 15);
    _setValue(EMS_RxTelegram, &EMS_Boiler.serviceCode, 20);

    // system pressure. FF means missing
    _setValue(EMS_RxTelegram, &EMS_Boiler.sysPress, 17); // is *10

    // read the service code / installation status as appears on the display
    if (EMS_RxTelegram->data_length > 18) {
        EMS_Boiler.serviceCodeChar[0] = char(EMS_RxTelegram->data[18]); // ascii character 1
        EMS_Boiler.serviceCodeChar[1] = char(EMS_RxTelegram->data[19]); // ascii character 2
        EMS_Boiler.serviceCodeChar[2] = '\0';                           // null terminate string
    }

    // at this point do a quick check to see if the hot water or heating is active
    _checkActive();
}

/**
 * UBAMonitorFast2 - type 0xE4 - central heating monitor
 */
void _process_UBAMonitorFast2(_EMS_RxTelegram * EMS_RxTelegram) {
    _setValue(EMS_RxTelegram, &EMS_Boiler.selFlowTemp, 6);
    _setValue(EMS_RxTelegram, &EMS_Boiler.burnGas, 11, 0);
    _setValue(EMS_RxTelegram, &EMS_Boiler.wWHeat, 11, 2);
    _setValue(EMS_RxTelegram, &EMS_Boiler.curBurnPow, 10);
    _setValue(EMS_RxTelegram, &EMS_Boiler.selBurnPow, 9);
    _setValue(EMS_RxTelegram, &EMS_Boiler.curFlowTemp, 7); // 0x8000 if not available
    _setValue(EMS_RxTelegram, &EMS_Boiler.flameCurr, 19);

    // read the service code / installation status as appears on the display
    if (EMS_RxTelegram->data_length > 4) {
        EMS_Boiler.serviceCodeChar[0] = char(EMS_RxTelegram->data[4]); // ascii character 1
        EMS_Boiler.serviceCodeChar[1] = char(EMS_RxTelegram->data[5]); // ascii character 2
        EMS_Boiler.serviceCodeChar[2] = '\0';
    }

    // still to figure out:
    //  EMS_Boiler.serviceCode
    //  EMS_Boiler.retTemp
    //  EMS_Boiler.sysPress

    // at this point do a quick check to see if the hot water or heating is active
    _checkActive();
}

/**
 * UBAMonitorSlow - type 0x19 - central heating monitor part 2 (27 bytes long)
 * received every 60 seconds
 * e.g. 08 00 19 00 80 00 02 41 80 00 00 00 00 00 03 91 7B 05 B8 40 00 00 00 04 92 AD 00 5E EE 80 00 (CRC=C9) #data=27
 *      08 0B 19 00 FF EA 02 47 80 00 00 00 00 62 03 CA 24 2C D6 23 00 00 00 27 4A B6 03 6E 43 
 *                  00 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 17 19 20 21 22 23 24
 */
void _process_UBAMonitorSlow(_EMS_RxTelegram * EMS_RxTelegram) {
    _setValue(EMS_RxTelegram, &EMS_Boiler.extTemp, 0);
    _setValue(EMS_RxTelegram, &EMS_Boiler.boilTemp, 2);
    _setValue(EMS_RxTelegram, &EMS_Boiler.switchTemp, 25); // only if there is a mixer
    _setValue(EMS_RxTelegram, &EMS_Boiler.pumpMod, 9);
    _setValue(EMS_RxTelegram, &EMS_Boiler.burnStarts, 10);
    _setValue(EMS_RxTelegram, &EMS_Boiler.burnWorkMin, 13);
    _setValue(EMS_RxTelegram, &EMS_Boiler.heatWorkMin, 19);
}

/**
 * UBAMonitorSlow2 - type 0xE5 - central heating monitor
 */
void _process_UBAMonitorSlow2(_EMS_RxTelegram * EMS_RxTelegram) {
    _setValue(EMS_RxTelegram, &EMS_Boiler.fanWork, 2, 2);
    _setValue(EMS_RxTelegram, &EMS_Boiler.ignWork, 2, 3);
    _setValue(EMS_RxTelegram, &EMS_Boiler.heatPmp, 2, 5);
    _setValue(EMS_RxTelegram, &EMS_Boiler.wWCirc, 2, 7);
    _setValue(EMS_RxTelegram, &EMS_Boiler.burnStarts, 10);
    _setValue(EMS_RxTelegram, &EMS_Boiler.burnWorkMin, 13);
    _setValue(EMS_RxTelegram, &EMS_Boiler.heatWorkMin, 19);
    _setValue(EMS_RxTelegram, &EMS_Boiler.pumpMod, 25); // or is it switchTemp ?
}

/**
 * UBAOutdoorTemp - type 0xD1 - external temperature
 */
void _process_UBAOutdoorTemp(_EMS_RxTelegram * EMS_RxTelegram) {
    _setValue(EMS_RxTelegram, &EMS_Boiler.extTemp, 0);
}

/**
 * type 0xB1 - data from the RC10 thermostat (0x17)
 * For reading the temp values only
 * received every 60 seconds
 * e.g. 17 0B 91 00 80 1E 00 CB 27 00 00 00 00 05 01 00 CB 00 (CRC=47), #data=14
 */
void _process_RC10StatusMessage(_EMS_RxTelegram * EMS_RxTelegram) {
    uint8_t hc                   = EMS_THERMOSTAT_DEFAULTHC - 1; // use HC1
    EMS_Thermostat.hc[hc].active = true;

    _setValue8(EMS_RxTelegram, &EMS_Thermostat.hc[hc].setpoint_roomTemp, EMS_OFFSET_RC10StatusMessage_setpoint); // is * 2, force as single byte
    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].curr_roomTemp, EMS_OFFSET_RC10StatusMessage_curr);          // is * 10
}

/**
 * type 0x91 - data from the RC20 thermostat (0x17) - 15 bytes long
 * For reading the temp values only
 * received every 60 seconds
 */
void _process_RC20StatusMessage(_EMS_RxTelegram * EMS_RxTelegram) {
    uint8_t hc                   = EMS_THERMOSTAT_DEFAULTHC - 1; // use HC1
    EMS_Thermostat.hc[hc].active = true;

    _setValue8(EMS_RxTelegram, &EMS_Thermostat.hc[hc].setpoint_roomTemp, EMS_OFFSET_RC20StatusMessage_setpoint); // is * 2, force as single byte
    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].curr_roomTemp, EMS_OFFSET_RC20StatusMessage_curr);          // is * 10
}

/**
 * type 0x41 - data from the RC30 thermostat(0x10) - 14 bytes long
 * For reading the temp values only * received every 60 seconds 
*/
void _process_RC30StatusMessage(_EMS_RxTelegram * EMS_RxTelegram) {
    uint8_t hc                   = EMS_THERMOSTAT_DEFAULTHC - 1; // use HC1
    EMS_Thermostat.hc[hc].active = true;

    _setValue8(EMS_RxTelegram, &EMS_Thermostat.hc[hc].setpoint_roomTemp, EMS_OFFSET_RC30StatusMessage_setpoint); // is * 2, force as single byte
    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].curr_roomTemp, EMS_OFFSET_RC30StatusMessage_curr);
}

/**
 * type 0x3E (HC1), 0x48 (HC2), 0x52 (HC3), 0x5C (HC4) - HK1MonitorMessage - data from the RC35 thermostat (0x10) - 16 bytes
 * examples:
 * broadcasts when manually setting HC2 to 14 degrees:
 * 10 00 3E 00   04 03 00 7D 00 00 00 00 00 00 00 00 00 11 05 00
 * 10 00 48 00   04 13 1C 00 E4 00 00 00 00 00 00 00 64 11 0F 00
 * 
 * 10 00 3E 00   80 02 1A 7D 00 00 00 00 00 00 00 00 64 11 16 00
 *
 * 10 0B 3E 00   00 00 00 7D 00 00 00 00 00 00 00 00 00 11 05 04
 * 10 00 48 00   00 00 00 00 E4 00 00 00 00 00 00 00 00 11 05 04
 * 10 0B 52 00   00 00 00 00 00 00 00 00 00 00 00 00 00 11 00 00
 * 10 0B 5C 00   00 00 00 00 00 00 00 00 00 00 00 00 00 11 00 00
 * 
 * night mode:
 * 10 00 3E 00   04 03 00 7D 00 00 00 00 00 00 00 00 00 11 05 00
 * 10 00 48 00   00 00 10 00 E9 00 00 00 00 00 00 00 00 11 05 00
 * 
 * day mode:
 * 10 0B 3E 00   04 03 00 7D 00 00 00 00 00 00 00 00 00 11 05 00
 * 10 0B 48 00   00 00 10 00 E8 00 00 00 00 00 00 00 00 11 05 00
 * 
 * auto day:
 * 10 00 3E 00   04 03 00 7D 00 00 00 00 00 00 00 00 00 11 05 00
 * 10 0B 48 00   04 03 00 00 EB 00 00 00 00 00 00 00 00 11 05 00
 * 
 * For reading the current room temperature only and picking up the modes
 * received every 60 seconds
 */
void _process_RC35StatusMessage(_EMS_RxTelegram * EMS_RxTelegram) {
    // exit if...
    // - the 15th byte (second from last) is 0x00, which I think is flow temp, means HC is not is use
    // - its not a broadcast, so destination is 0x00
    if ((EMS_RxTelegram->data[14] == 0x00) || (EMS_RxTelegram->dest != EMS_ID_NONE)) {
        return;
    }

    uint8_t hc_num = _getHeatingCircuit(EMS_RxTelegram); // which HC is it, 0-3

    // ignore if the value is 0 (see https://github.com/proddy/EMS-ESP/commit/ccc30738c00f12ae6c89177113bd15af9826b836)
    if (EMS_RxTelegram->data[EMS_OFFSET_RC35StatusMessage_setpoint] != 0x00) {
        _setValue8(EMS_RxTelegram, &EMS_Thermostat.hc[hc_num].setpoint_roomTemp, EMS_OFFSET_RC35StatusMessage_setpoint); // is * 2, force to single byte
    }

    // ignore if the value is unset. Hopefully it will be picked up via a later message
    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc_num].curr_roomTemp, EMS_OFFSET_RC35StatusMessage_curr); // is * 10
    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc_num].day_mode, EMS_OFFSET_RC35StatusMessage_mode, 1);
    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc_num].summer_mode, EMS_OFFSET_RC35StatusMessage_mode, 0);
    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc_num].holiday_mode, EMS_OFFSET_RC35StatusMessage_mode1, 5);
    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc_num].circuitcalctemp, EMS_OFFSET_RC35Set_circuitcalctemp);
}

/**
 * type 0x0A - data from the Nefit Easy/TC100 thermostat (0x18) - 31 bytes long
 * The Easy has a digital precision of its floats to 2 decimal places, so values must be divided by 100
 */
void _process_EasyStatusMessage(_EMS_RxTelegram * EMS_RxTelegram) {
    uint8_t hc                   = EMS_THERMOSTAT_DEFAULTHC - 1; // use HC1
    EMS_Thermostat.hc[hc].active = true;

    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].curr_roomTemp, EMS_OFFSET_EasyStatusMessage_curr);         // is * 100
    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].setpoint_roomTemp, EMS_OFFSET_EasyStatusMessage_setpoint); // is * 100
}

void _process_MMPLUSStatusMessage(_EMS_RxTelegram * EMS_RxTelegram) {
    uint8_t hc = (EMS_RxTelegram->type - EMS_TYPE_MMPLUSStatusMessage_HC1); // 0 to 3
    if (hc >= EMS_THERMOSTAT_MAXHC) {
        return; // invalid type
    }
    EMS_Mixing.hc[hc].active = true;

    _setValue(EMS_RxTelegram, &EMS_Mixing.hc[hc].flowTemp, EMS_OFFSET_MMPLUSStatusMessage_flow_temp);
    _setValue(EMS_RxTelegram, &EMS_Mixing.hc[hc].pumpMod, EMS_OFFSET_MMPLUSStatusMessage_pump_mod);
    _setValue(EMS_RxTelegram, &EMS_Mixing.hc[hc].valveStatus, EMS_OFFSET_MMPLUSStatusMessage_valve_status);
}

/**
 * type 0x01A5 - data from the Nefit RC1010/3000 thermostat (0x18) and RC300/310s on 0x10
 * EMS+ messages may come in with different offsets so handle them here
 */
void _process_RCPLUSStatusMessage(_EMS_RxTelegram * EMS_RxTelegram) {
    // figure out which heating circuit
    uint8_t hc = (EMS_RxTelegram->type - EMS_TYPE_RCPLUSStatusMessage_HC1); // 0 to 3
    if (hc >= EMS_THERMOSTAT_MAXHC) {
        return; // invalid type
    }
    EMS_Thermostat.hc[hc].active = true;

    // handle single data values. data will always be at position data[0]
    if (EMS_RxTelegram->data_length == 1) {
        switch (EMS_RxTelegram->offset) {
        case EMS_OFFSET_RCPLUSStatusMessage_curr:                               // setpoint target temp
            _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].curr_roomTemp, 0); // value is * 10
            break;
        case EMS_OFFSET_RCPLUSStatusMessage_setpoint:                          // current target temp
            EMS_Thermostat.hc[hc].setpoint_roomTemp = EMS_RxTelegram->data[0]; // convert to single byte, value is * 2
            break;
        case EMS_OFFSET_RCPLUSStatusMessage_currsetpoint:                      // current setpoint temp,  e.g. Thermostat -> all, telegram: 10 00 FF 06 01 A5 22
            EMS_Thermostat.hc[hc].setpoint_roomTemp = EMS_RxTelegram->data[0]; // convert to single byte, value is * 2
            break;
        case EMS_OFFSET_RCPLUSStatusMessage_mode: // thermostat mode auto/manual
                                                  // manual : 10 00 FF 0A 01 A5 02
                                                  // auto :   10 00 FF 0A 01 A5 03
            _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].mode, 0,
                      0); // bit 1, mode (auto=1 or manual=0). Note this may be bit 2 - still need to validate
            _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].day_mode, 0, 1); // get day mode flag

            break;
        }
    } else if (EMS_RxTelegram->data_length > 20) {
        // the whole telegram
        // e.g. Thermostat -> all, telegram: 10 00 FF 00 01 A5 00 D7 21 00 00 00 00 30 01 84 01 01 03 01 84 01 F1 00 00 11 01 00 08 63 00
        //                                   10 00 FF 00 01 A5 80 00 01 30 28 00 30 28 01 54 03 03 01 01 54 02 A8 00 00 11 01 03 FF FF 00
        _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].curr_roomTemp, EMS_OFFSET_RCPLUSStatusMessage_curr);          // value is * 10
        _setValue8(EMS_RxTelegram, &EMS_Thermostat.hc[hc].setpoint_roomTemp, EMS_OFFSET_RCPLUSStatusMessage_setpoint); // convert to single byte, value is * 2
        _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].day_mode, EMS_OFFSET_RCPLUSStatusMessage_mode, 1);
        _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].mode, EMS_OFFSET_RCPLUSStatusMessage_mode, 0); // bit 1, mode (auto=1 or manual=0)
    }
}

/**
 * type 0x01AF - summer/winter mode from the Nefit RC1010 thermostat (0x18) and RC300/310s on 0x10
 */
void _process_RCPLUSStatusMode(_EMS_RxTelegram * EMS_RxTelegram) {
    // data[0] // 0x00=OFF 0x01=Automatic 0x02=Forced
}

/**
 * FR10/FR50/FR100 Junkers - type x006F
 *    e.g. for FR10:  90 00 FF 00 00 6F   03 01 00 BE 00 BF
 *         for FW100: 90 00 FF 00 00 6F   03 02 00 D7 00 DA F3 34 00 C4
 */
void _process_JunkersStatusMessage(_EMS_RxTelegram * EMS_RxTelegram) {
    if (EMS_RxTelegram->offset == 0 && EMS_RxTelegram->data_length > 1) {
        uint8_t hc                   = EMS_THERMOSTAT_DEFAULTHC - 1; // use HC1
        EMS_Thermostat.hc[hc].active = true;

        _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].curr_roomTemp, EMS_OFFSET_JunkersStatusMessage_curr);         // value is * 10
        _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].setpoint_roomTemp, EMS_OFFSET_JunkersStatusMessage_setpoint); // value is * 10
        _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].day_mode, EMS_OFFSET_JunkersStatusMessage_daymode);           // 3 = day, 2 = night
        _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].mode, EMS_OFFSET_JunkersStatusMessage_mode);                  // 1 = manual, 2 = auto
    }
}

/**
 * type 0x01B9 EMS+ for reading the mode from RC300/RC310 thermostat
 */
void _process_RCPLUSSetMessage(_EMS_RxTelegram * EMS_RxTelegram) {
    // ignore F7 and F9
    if (EMS_RxTelegram->emsplus_type != 0xFF) {
        return;
    }

    uint8_t hc                   = EMS_THERMOSTAT_DEFAULTHC - 1; // use HC1
    EMS_Thermostat.hc[hc].active = true;

    // check for one data value
    // but ignore values of 0xFF, e.g.  10 00 FF 08 01 B9 FF
    if ((EMS_RxTelegram->data_length == 1) && (EMS_RxTelegram->data[0] != 0xFF)) {
        // check for setpoint temps, e.g. Thermostat -> all, type 0x01B9, telegram: 10 00 FF 08 01 B9 26
        if ((EMS_RxTelegram->offset == EMS_OFFSET_RCPLUSSet_temp_setpoint) || (EMS_RxTelegram->offset == EMS_OFFSET_RCPLUSSet_manual_setpoint)) {
            _setValue8(EMS_RxTelegram, &EMS_Thermostat.hc[hc].setpoint_roomTemp, 0); // single byte conversion, value is * 2
        } else if (EMS_RxTelegram->offset == EMS_OFFSET_RCPLUSSet_mode) {
            // check for mode, eg.  10 00 FF 08 01 B9 FF
            EMS_Thermostat.hc[hc].mode = (EMS_RxTelegram->data[0] == 0xFF); // Auto = xFF, Manual = x00   (auto=1 or manual=0)
        }
        return; // quit
    }

    // check for long broadcasts
    if (EMS_RxTelegram->offset == 0) {
        _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].mode, EMS_OFFSET_RCPLUSSet_mode);
        _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].daytemp, EMS_OFFSET_RCPLUSSet_temp_comfort2); // is * 2
        _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].nighttemp, EMS_OFFSET_RCPLUSSet_temp_eco);    // is * 2
    }
}

/**
 * type 0xB0 - for reading the mode from the RC10 thermostat (0x17)
 * received only after requested
 */
void _process_RC10Set(_EMS_RxTelegram * EMS_RxTelegram) {
    // mode not implemented yet
}

/**
 * type 0xA8 - for reading the mode from the RC20 thermostat (0x17)
 * received only after requested
 */
void _process_RC20Set(_EMS_RxTelegram * EMS_RxTelegram) {
    uint8_t hc                   = EMS_THERMOSTAT_DEFAULTHC - 1; // use HC1
    EMS_Thermostat.hc[hc].active = true;
    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].mode, EMS_OFFSET_RC20Set_mode); // note, fixed for HC1
}

/**
 * type 0xA7 - for reading the mode from the RC30 thermostat (0x10)
 * received only after requested
 */
void _process_RC30Set(_EMS_RxTelegram * EMS_RxTelegram) {
    uint8_t hc                   = EMS_THERMOSTAT_DEFAULTHC - 1; // use HC1
    EMS_Thermostat.hc[hc].active = true;
    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc].mode, EMS_OFFSET_RC30Set_mode); // note, fixed for HC1
}

// return which heating circuit it is, 0-3 for HC1 to HC4
// based on type 0x3E (HC1), 0x48 (HC2), 0x52 (HC3), 0x5C (HC4)
uint8_t _getHeatingCircuit(_EMS_RxTelegram * EMS_RxTelegram) {
    uint8_t hc_num;
    switch (EMS_RxTelegram->type) {
    case EMS_TYPE_RC35StatusMessage_HC1:
    case EMS_TYPE_RC35Set_HC1:
    default:
        hc_num = 1; // also default
        break;
    case EMS_TYPE_RC35StatusMessage_HC2:
    case EMS_TYPE_RC35Set_HC2:
        hc_num = 2;
        break;
    case EMS_TYPE_RC35StatusMessage_HC3:
    case EMS_TYPE_RC35Set_HC3:
        hc_num = 3;
        break;
    case EMS_TYPE_RC35StatusMessage_HC4:
    case EMS_TYPE_RC35Set_HC4:
        hc_num = 4;
        break;
    }

    hc_num--;
    EMS_Thermostat.hc[hc_num].active = true;

    return (hc_num);
}

/**
 * type 0x3D (HC1), 0x47 (HC2), 0x51 (HC3), 0x5B (HC4) - Working Mode Heating - for reading the mode from the RC35 thermostat (0x10)
 * received only after requested
 * 10 0B 47 00  03 13 15 26 0A 28 00 02 00 05 05 2D 01 01 04 4B 05 4B 01 00 3C FF 0D 05 05 02 02
 * 10 0B 3D 00  01 2B 39 26 00 28 00 02 00 05 05 2D 01 01 04 3C 06 39 01 00 3C FF 0D 05 05 03 00
 * 10 0B 51 00  00 13 15 26 00 28 00 02 00 05 05 2D 01 01 04 4B 05 4B 01 00 3C FF 11 05 05 03 02
 * 10 0B 5B 00  00 13 15 26 00 28 00 02 00 05 05 2D 01 01 04 4B 05 4B 01 00 3C FF 11 05 05 03 02
 */
void _process_RC35Set(_EMS_RxTelegram * EMS_RxTelegram) {
    // check to see we have a valid type
    // heating: 1 radiator, 2 convectors, 3 floors, 4 room supply
    if (EMS_RxTelegram->data[0] == 0x00) {
        return;
    }

    uint8_t hc_num = _getHeatingCircuit(EMS_RxTelegram); // which HC is it?

    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc_num].mode, EMS_OFFSET_RC35Set_mode);                // night, day, auto
    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc_num].daytemp, EMS_OFFSET_RC35Set_temp_day);         // is * 2
    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc_num].nighttemp, EMS_OFFSET_RC35Set_temp_night);     // is * 2
    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc_num].holidaytemp, EMS_OFFSET_RC35Set_temp_holiday); // is * 2
    _setValue(EMS_RxTelegram, &EMS_Thermostat.hc[hc_num].heatingtype, EMS_OFFSET_RC35Set_heatingtype);  // byte 0 bit floor heating = 3
}

/**
 * type 0xA3 - for external temp settings from the the RC* thermostats
 */
void _process_RCOutdoorTempMessage(_EMS_RxTelegram * EMS_RxTelegram) {
    // add support here if you're reading external sensors
}

/*
 * SM10Monitor - type 0x97
 */
void _process_SM10Monitor(_EMS_RxTelegram * EMS_RxTelegram) {
    _setValue(EMS_RxTelegram, &EMS_SolarModule.collectorTemp, 2);  // collector temp from SM10, is *10
    _setValue(EMS_RxTelegram, &EMS_SolarModule.bottomTemp, 5);     // bottom temp from SM10, is *10
    _setValue(EMS_RxTelegram, &EMS_SolarModule.pumpModulation, 4); // modulation solar pump
    _setValue(EMS_RxTelegram, &EMS_SolarModule.pump, 7, 1);        // active if bit 1 is set
}

/*
 * SM100Monitor - type 0x0262 EMS+
 * e.g, 30 00 FF 00 02 62 01 AC
 *      30 00 FF 18 02 62 80 00
 *      30 00 FF 00 02 62 01 A1 - for bottom temps
 */
void _process_SM100Monitor(_EMS_RxTelegram * EMS_RxTelegram) {
    // only process the complete telegram, not partial
    if (EMS_RxTelegram->offset) {
        return;
    }

    _setValue(EMS_RxTelegram, &EMS_SolarModule.collectorTemp, 0); // is *10
    _setValue(EMS_RxTelegram, &EMS_SolarModule.bottomTemp, 2);    // is *10
}

/*
 * SM100Status - type 0x0264 EMS+ for pump modulation
 * e.g. 30 00 FF 09 02 64 64 = 100%
 *      30 00 FF 09 02 64 1E = 30%
 */
void _process_SM100Status(_EMS_RxTelegram * EMS_RxTelegram) {
    if (EMS_RxTelegram->offset == 0) {
        _setValue(EMS_RxTelegram, &EMS_SolarModule.pumpModulation, 9); // check for complete telegram
    } else if (EMS_RxTelegram->offset == 0x09) {
        _setValue(EMS_RxTelegram, &EMS_SolarModule.pumpModulation, 0); // data at offset 09
    }
}

/*
 * SM100Status2 - type 0x026A EMS+ for pump on/off at offset 0x0A
 */
void _process_SM100Status2(_EMS_RxTelegram * EMS_RxTelegram) {
    if (EMS_RxTelegram->offset == 0) {
        _setValue(EMS_RxTelegram, &EMS_SolarModule.pump, 10, 2); // 03=off 04=on
    } else if (EMS_RxTelegram->offset == 0x0A) {
        _setValue(EMS_RxTelegram, &EMS_SolarModule.pump, 0, 2); // 03=off 04=on at offset 0A
    }
}

/*
 * SM100Energy - type 0x028E EMS+ for energy readings
 * e.g. 30 00 FF 00 02 8E 00 00 00 00 00 00 06 C5 00 00 76 35
 */
void _process_SM100Energy(_EMS_RxTelegram * EMS_RxTelegram) {
    _setValue(EMS_RxTelegram, &EMS_SolarModule.EnergyLastHour, 2); // last hour / 10 in Wh
    _setValue(EMS_RxTelegram, &EMS_SolarModule.EnergyToday, 6);    //  todays in Wh
    _setValue(EMS_RxTelegram, &EMS_SolarModule.EnergyTotal, 10);   //  total / 10 in kWh
}

/*
 * Type 0xE3 - HeatPump Monitor 1
 */
void _process_HPMonitor1(_EMS_RxTelegram * EMS_RxTelegram) {
    _setValue(EMS_RxTelegram, &EMS_HeatPump.HPModulation, 13); // %
}

/*
 * Type 0xE5 - HeatPump Monitor 2
 */
void _process_HPMonitor2(_EMS_RxTelegram * EMS_RxTelegram) {
    _setValue(EMS_RxTelegram, &EMS_HeatPump.HPSpeed, 25); // %
}

/*
 * Junkers ISM1 Solar Module - type 0x0003 EMS+ for energy readings
 *  e.g. B0 00 FF 00 00 03 32 00 00 00 00 13 00 D6 00 00 00 FB D0 F0
 */
void _process_ISM1StatusMessage(_EMS_RxTelegram * EMS_RxTelegram) {
    if (EMS_RxTelegram->offset == 0) {
        _setValue(EMS_RxTelegram, &EMS_SolarModule.collectorTemp, 4);  // Collector Temperature
        _setValue(EMS_RxTelegram, &EMS_SolarModule.bottomTemp, 6);     // Temperature Bottom of Solar Boiler
        _setValue(EMS_RxTelegram, &EMS_SolarModule.EnergyLastHour, 2); // Solar Energy produced in last hour - is * 10 and handled in ems-esp.cpp
        _setValue(EMS_RxTelegram, &EMS_SolarModule.pump, 8, 0);        // Solar pump on (1) or off (0)
        _setValue(EMS_RxTelegram, &EMS_SolarModule.pumpWorkMin, 10);
    }

    if (EMS_RxTelegram->offset == 4) {
        // e.g. B0 00 FF 04 00 03 02 E5
        _setValue(EMS_RxTelegram, &EMS_SolarModule.collectorTemp, 0); // Collector Temperature
    }
}


/*
 * Junkers ISM1 Solar Module - type 0x0001 EMS+ for setting values
 */
void _process_ISM1Set(_EMS_RxTelegram * EMS_RxTelegram) {
    if (EMS_RxTelegram->offset == 6) {
        // e.g. 90 30 FF 06 00 01 50 (CRC=2C)
        // to implement: change max solar boiler temperature
        EMS_SolarModule.setpoint_maxBottomTemp = EMS_RxTelegram->data[0];
    }
}

/**
 * UBASetPoint 0x1A
 */
void _process_SetPoints(_EMS_RxTelegram * EMS_RxTelegram) {
    if (EMS_Sys_Status.emsLogging == EMS_SYS_LOGGING_VERBOSE) {
        if (EMS_RxTelegram->data_length) {
            uint8_t setpoint = EMS_RxTelegram->data[0]; // flow temp
            //uint8_t ww_power = data[2]; // power in %

            /* use this logic if the value is *2
            char s[5];
            char s2[5];
            strlcpy(s, itoa(setpoint >> 1, s2, 10), 5);
            strlcat(s, ".", sizeof(s));
            strlcat(s, ((setpoint & 0x01) ? "5" : "0"), 5);
            myDebug_P(PSTR(" Boiler flow temp %s C, Warm Water power %d %"), s, ww_power);
            */

            myDebug_P(PSTR(" Boiler flow temperature is %d C"), setpoint);
        }
    }
}

/**
 * process_RCTime - type 0x06 - date and time from a thermostat - 14 bytes long
 * common for all thermostats
 */
void _process_RCTime(_EMS_RxTelegram * EMS_RxTelegram) {
    if ((EMS_Thermostat.device_flags == EMS_DEVICE_FLAG_EASY)) {
        return; // not supported
    }

    // render time to HH:MM:SS DD/MM/YYYY
    char time_sp[25];
    char buffer[4];

    strlcpy(time_sp, _smallitoa(EMS_RxTelegram->data[2], buffer), sizeof(time_sp)); // hour
    strlcat(time_sp, ":", sizeof(time_sp));
    strlcat(time_sp, _smallitoa(EMS_RxTelegram->data[4], buffer), sizeof(time_sp)); // minute
    strlcat(time_sp, ":", sizeof(time_sp));
    strlcat(time_sp, _smallitoa(EMS_RxTelegram->data[5], buffer), sizeof(time_sp)); // second
    strlcat(time_sp, " ", sizeof(time_sp));
    strlcat(time_sp, _smallitoa(EMS_RxTelegram->data[3], buffer), sizeof(time_sp)); // day
    strlcat(time_sp, "/", sizeof(time_sp));
    strlcat(time_sp, _smallitoa(EMS_RxTelegram->data[1], buffer), sizeof(time_sp)); // month
    strlcat(time_sp, "/", sizeof(time_sp));
    strlcat(time_sp, itoa(EMS_RxTelegram->data[0] + 2000, buffer, 10), sizeof(time_sp)); // year

    strlcpy(EMS_Thermostat.datetime, time_sp, sizeof(time_sp)); // store
}

/*
 * Clear devices list
 */
void ems_clearDeviceList() {
    Devices.clear();

    for (uint8_t i = 0; i < EMS_SYS_DEVICEMAP_LENGTH; i++) {
        EMS_Sys_Status.emsDeviceMap[i] = 0x00;
    }
}

/*
 * add an EMS device to our list of detected devices if its unique
 * returns true if already in list
 */
bool _addDevice(_EMS_DEVICE_TYPE device_type, uint8_t product_id, uint8_t device_id, const char * device_desc_p, const char * version) {
    _Detected_Device device;

    // check for duplicates
    // a combi of product_id and device_id make it unique
    for (std::list<_Detected_Device>::iterator it = Devices.begin(); it != Devices.end(); ++it) {
        if (((it)->product_id == product_id) && ((it)->device_id == device_id)) {
            return (true); // it already exists in the list, don't add
        }
    }

    // create a new record and add it to list
    device.device_type   = device_type;
    device.product_id    = product_id;
    device.device_id     = device_id;
    device.device_desc_p = device_desc_p; // pointer to the description in the EMS_Devices table
    strlcpy(device.version, version, sizeof(device.version));
    device.known = (device_type != EMS_DEVICE_TYPE_UNKNOWN);
    Devices.push_back(device);

    char line[500];
    strlcpy(line, "New EMS device recognized as a ", sizeof(line));

    // get type as a string
    char type_s[50];
    if (ems_getDeviceTypeDescription(device_id, type_s)) {
        strlcat(line, type_s, sizeof(line));
    } else {
        strlcat(line, "?", sizeof(line));
    }

    char tmp[6] = {0}; // for formatting numbers

    if (device_desc_p != nullptr) {
        strlcat(line, ": ", sizeof(line));
        strlcat(line, device_desc_p, sizeof(line));
    }

    strlcat(line, " (DeviceID:0x", sizeof(line));
    strlcat(line, _hextoa(device_id, tmp), sizeof(line));
    strlcat(line, " ProductID:", sizeof(line));
    strlcat(line, itoa(product_id, tmp, 10), sizeof(line));
    strlcat(line, " Version:", sizeof(line));
    strlcat(line, version, sizeof(line));
    strlcat(line, ")", sizeof(line));

    myDebug(line); // print it

    return false; // added, wasn't a duplicate
}

/**
 * type 0x07 - shows us the connected EMS devices
 * e.g. 08 00 07 00 0B 80 00 00 00 00 00 00 00 00 00 00 00 (CRC=47) #data=13  
 * Junkers is 15 (I think)
 */
void _process_UBADevices(_EMS_RxTelegram * EMS_RxTelegram) {
    if (EMS_RxTelegram->data_length > EMS_SYS_DEVICEMAP_LENGTH) {
        return; // should be 13 or 15 bytes long
    }

    for (uint8_t data_byte = 0; data_byte < EMS_RxTelegram->data_length; data_byte++) {
        uint8_t byte       = EMS_RxTelegram->data[data_byte];
        uint8_t saved_byte = EMS_Sys_Status.emsDeviceMap[data_byte];

        // see if this matches what we already have stored
        if (byte != saved_byte) {
            // we have something new
            EMS_Sys_Status.emsDeviceMap[data_byte] = byte; // save new value
            // go through all bits
            // myDebug("Byte #%d 0x%02X", data_byte, byte); // for debugging
            if (byte) {
                for (uint8_t bit = 0; bit < 8; bit++) {
                    if ((byte & 0x01) && ((saved_byte & 0x01) == 0)) {
                        uint8_t device_id = ((data_byte + 1) * 8) + bit;
                        if (device_id != EMS_ID_ME) {
                            // myDebug("[EMS] Detected new EMS Device with ID 0x%02X", device_id);
                            if (!ems_getTxDisabled()) {
                                ems_doReadCommand(EMS_TYPE_Version, device_id); // get version, but ignore ourselves
                            }
                        }
                    }
                    byte       = byte >> 1;
                    saved_byte = saved_byte >> 1;
                }
            }
        }
    }
}

/**
 * type 0x02 - get the version and type of an EMS device
 * look up known devices via the product id and make it active if not already setup
 */
void _process_Version(_EMS_RxTelegram * EMS_RxTelegram) {
    // ignore short messages that we can't interpret
    if (EMS_RxTelegram->data_length < 3) {
        return;
    }

    uint8_t offset = 0;

    // check for 2nd subscriber
    // e.g. 18 0B 02 00 00 00 00 5E 02 01
    if (EMS_RxTelegram->data[0] == 0x00) {
        // see if we have a 2nd subscriber
        if (EMS_RxTelegram->data[3] != 0x00) {
            offset = 3;
        } else {
            return; // ignore whole telegram
        }
    }

    uint8_t device_id = EMS_RxTelegram->src; // device ID

    // get version as XX.XX
    char version[10] = {0};
    char buf[6]      = {0};
    strlcpy(version, _smallitoa(EMS_RxTelegram->data[offset + 1], buf), sizeof(version));
    strlcat(version, ".", sizeof(version));
    strlcat(version, _smallitoa(EMS_RxTelegram->data[offset + 2], buf), sizeof(version));

    // scan through known devices matching the productid
    uint8_t product_id = EMS_RxTelegram->data[offset];
    uint8_t i          = 0;
    bool    typeFound  = false;
    while (i < _EMS_Devices_max) {
        if (EMS_Devices[i].product_id == product_id) {
            typeFound = true; // we have a matching product id. i is the index.
            break;
        }
        i++;
    }

    // if not found, just add it
    if (!typeFound) {
        (void)_addDevice(EMS_DEVICE_TYPE_UNKNOWN, product_id, device_id, nullptr, version);
        return;
    }

    const char *     device_desc_p = (EMS_Devices[i].device_desc); // pointer to the full description of the device
    _EMS_DEVICE_TYPE type          = EMS_Devices[i].type;          // type

    // we recognized it, see if we already have it in our recognized list
    if (_addDevice(type, product_id, device_id, device_desc_p, version)) {
        return; // already in list
    }

    uint8_t flags = EMS_Devices[i].flags; // its a new entry, set the specifics

    if (type == EMS_DEVICE_TYPE_BOILER) {
        EMS_Boiler.device_id     = device_id;
        EMS_Boiler.product_id    = product_id;
        EMS_Boiler.device_flags  = flags;
        EMS_Boiler.device_desc_p = device_desc_p;
        strlcpy(EMS_Boiler.version, version, sizeof(EMS_Boiler.version));
        ems_getBoilerValues(); // get Boiler values that we would usually have to wait for
    } else if (type == EMS_DEVICE_TYPE_THERMOSTAT) {
        EMS_Thermostat.device_id       = device_id;
        EMS_Thermostat.device_flags    = (flags & 0x7F); // remove 7th bit
        EMS_Thermostat.write_supported = (flags & EMS_DEVICE_FLAG_NO_WRITE) == 0;
        EMS_Thermostat.product_id      = product_id;
        EMS_Thermostat.device_desc_p   = device_desc_p;
        strlcpy(EMS_Thermostat.version, version, sizeof(EMS_Thermostat.version));
        ems_getThermostatValues(); // get Thermostat values
    } else if (type == EMS_DEVICE_TYPE_SOLAR) {
        EMS_SolarModule.device_id     = device_id;
        EMS_SolarModule.product_id    = product_id;
        EMS_SolarModule.device_flags  = flags;
        EMS_SolarModule.device_desc_p = device_desc_p;
        strlcpy(EMS_SolarModule.version, version, sizeof(EMS_SolarModule.version));
        ems_getSolarModuleValues(); // fetch Solar Module values
    } else if (type == EMS_DEVICE_TYPE_HEATPUMP) {
        EMS_HeatPump.device_id     = device_id;
        EMS_HeatPump.product_id    = product_id;
        EMS_HeatPump.device_flags  = flags;
        EMS_HeatPump.device_desc_p = device_desc_p;
        strlcpy(EMS_HeatPump.version, version, sizeof(EMS_HeatPump.version));
    } else if (type == EMS_DEVICE_TYPE_MIXING) {
        EMS_Mixing.device_id     = device_id;
        EMS_Mixing.product_id    = product_id;
        EMS_Mixing.device_desc_p = device_desc_p;
        EMS_Mixing.device_flags  = flags;
        EMS_Mixing.detected      = true;
        ems_doReadCommand(EMS_TYPE_MMPLUSStatusMessage_HC1, device_id); // fetch MM values
    }
}

/*
 * Figure out the boiler and thermostat types
 */
void ems_discoverModels() {
    //myDebug_P(PSTR("Starting auto discover of EMS devices..."));
    ems_doReadCommand(EMS_TYPE_UBADevices, EMS_ID_BOILER);
}

/**
 * Print the Tx queue - for debugging
 */
void ems_printTxQueue() {
    _EMS_TxTelegram EMS_TxTelegram;
    char            sType[20] = {0};

    if (EMS_TxQueue.size() == 0) {
        myDebug_P(PSTR("Tx queue is empty"));
        return;
    }

    myDebug_P(PSTR("Tx queue (%d/%d)"), EMS_TxQueue.size(), EMS_TxQueue.capacity);

    for (byte i = 0; i < EMS_TxQueue.size(); i++) {
        EMS_TxTelegram = EMS_TxQueue[i]; // retrieves the i-th element from the buffer without removing it

        // get action
        if (EMS_TxTelegram.action == EMS_TX_TELEGRAM_WRITE) {
            strlcpy(sType, "write", sizeof(sType));
        } else if (EMS_TxTelegram.action == EMS_TX_TELEGRAM_READ) {
            strlcpy(sType, "read", sizeof(sType));
        } else if (EMS_TxTelegram.action == EMS_TX_TELEGRAM_VALIDATE) {
            strlcpy(sType, "validate", sizeof(sType));
        } else if (EMS_TxTelegram.action == EMS_TX_TELEGRAM_RAW) {
            strlcpy(sType, "raw", sizeof(sType));
        } else {
            strlcpy(sType, "?", sizeof(sType));
        }

        char     addedTime[15] = {0};
        uint32_t upt           = EMS_TxTelegram.timestamp;
        snprintf(addedTime,
                 sizeof(addedTime),
                 "(%02d:%02d:%02d)",
                 (uint8_t)((upt / (1000 * 60 * 60)) % 24),
                 (uint8_t)((upt / (1000 * 60)) % 60),
                 (uint8_t)((upt / 1000) % 60));

        myDebug_P(PSTR(" [%d] action=%s dest=0x%02x type=0x%02x offset=%d length=%d dataValue=%d "
                       "comparisonValue=%d type_validate=0x%02x comparisonPostRead=0x%02x @ %s"),
                  i + 1,
                  sType,
                  EMS_TxTelegram.dest & 0x7F,
                  EMS_TxTelegram.type,
                  EMS_TxTelegram.offset,
                  EMS_TxTelegram.length,
                  EMS_TxTelegram.dataValue,
                  EMS_TxTelegram.comparisonValue,
                  EMS_TxTelegram.type_validate,
                  EMS_TxTelegram.comparisonPostRead,
                  addedTime);
    }
}

/**
 * Generic function to return various settings from the thermostat
 * This is called manually to fetch values which don't come from broadcast messages
 */
void ems_getThermostatValues() {
    if (!ems_getThermostatEnabled()) {
        return;
    }

    uint8_t device_flags = EMS_Thermostat.device_flags;
    uint8_t device_id    = EMS_Thermostat.device_id;
    uint8_t statusMsg, opMode;

    switch (device_flags) {
    case EMS_DEVICE_FLAG_RC20:
        ems_doReadCommand(EMS_TYPE_RC20StatusMessage, device_id); // to get the temps
        ems_doReadCommand(EMS_TYPE_RC20Set, device_id);           // to get the mode
        break;
    case EMS_DEVICE_FLAG_RC30:
        ems_doReadCommand(EMS_TYPE_RC30StatusMessage, device_id); // to get the temps
        ems_doReadCommand(EMS_TYPE_RC30Set, device_id);           // to get the mode
        break;
    case EMS_DEVICE_FLAG_EASY:
        ems_doReadCommand(EMS_TYPE_EasyStatusMessage, device_id);
        break;
    case EMS_DEVICE_FLAG_RC35:
        for (uint8_t hc_num = 1; hc_num <= EMS_THERMOSTAT_MAXHC; hc_num++) {
            if (hc_num == 1) {
                statusMsg = EMS_TYPE_RC35StatusMessage_HC1;
                opMode    = EMS_TYPE_RC35Set_HC1;
            } else if (hc_num == 2) {
                statusMsg = EMS_TYPE_RC35StatusMessage_HC2;
                opMode    = EMS_TYPE_RC35Set_HC2;
            } else if (hc_num == 3) {
                statusMsg = EMS_TYPE_RC35StatusMessage_HC3;
                opMode    = EMS_TYPE_RC35Set_HC3;
            } else if (hc_num == 4) {
                statusMsg = EMS_TYPE_RC35StatusMessage_HC4;
                opMode    = EMS_TYPE_RC35Set_HC4;
            }
            ems_doReadCommand(statusMsg, device_id); // to get the temps
            ems_doReadCommand(opMode, device_id);    // to get the mode
        }
        break;
    case EMS_DEVICE_FLAG_RC300:
        ems_doReadCommand(EMS_TYPE_RCPLUSStatusMessage_HC1, device_id);
        ems_doReadCommand(EMS_TYPE_RCPLUSStatusMessage_HC2, device_id);
        ems_doReadCommand(EMS_TYPE_RCPLUSStatusMessage_HC3, device_id);
        ems_doReadCommand(EMS_TYPE_RCPLUSStatusMessage_HC4, device_id);
    default:
        break;
    }

    ems_doReadCommand(EMS_TYPE_RCTime, device_id); // get Thermostat time
}

/**
 * Generic function to return various settings from the thermostat
 */
void ems_getBoilerValues() {
    ems_doReadCommand(EMS_TYPE_UBAMonitorFast, EMS_Boiler.device_id);        // get boiler stats, instead of waiting 10secs for the broadcast
    ems_doReadCommand(EMS_TYPE_UBAMonitorSlow, EMS_Boiler.device_id);        // get more boiler stats, instead of waiting 60secs for the broadcast
    ems_doReadCommand(EMS_TYPE_UBAParameterWW, EMS_Boiler.device_id);        // get Warm Water values
    ems_doReadCommand(EMS_TYPE_UBAParametersMessage, EMS_Boiler.device_id);  // get MC10 boiler values
    ems_doReadCommand(EMS_TYPE_UBATotalUptimeMessage, EMS_Boiler.device_id); // get uptime from boiler
}

/*
 * Get other values from EMS devices
 */
void ems_getSolarModuleValues() {
    if (ems_getSolarModuleEnabled()) {
        if (EMS_SolarModule.device_flags == EMS_DEVICE_FLAG_SM10) {
            ems_doReadCommand(EMS_TYPE_SM10Monitor, EMS_ID_SM); // fetch all from SM10Monitor
        } else if (EMS_SolarModule.device_flags == EMS_DEVICE_FLAG_SM100) {
            ems_doReadCommand(EMS_TYPE_SM100Monitor, EMS_ID_SM); // fetch all from SM100Monitor
        }
    }
}

/**
 * takes a device_id and tries to find the corresponding type name (e.g. Boiler)
 * If it can't find it, it will use the hex value and function returns false
 */
bool ems_getDeviceTypeDescription(uint8_t device_id, char * buffer) {
    uint8_t i         = 0;
    bool    typeFound = false;

    // scan through known ID types
    while (i < _EMS_Devices_Types_max) {
        if (EMS_Devices_Types[i].device_id == device_id) {
            typeFound = true; // we have a match
            break;
        }
        i++;
    }

    if (typeFound) {
        strlcpy(buffer, EMS_Devices_Types[i].device_type_string, 30);
        return true;
    } else {
        // print as hex value
        char hexbuffer[16] = {0};
        strlcpy(buffer, "0x", 30);
        strlcat(buffer, _hextoa(device_id, hexbuffer), 30);
        return false;
    }
}


/**
 * returns current device details as a string for known thermostat,boiler,solar and heatpump
 */
char * ems_getDeviceDescription(_EMS_DEVICE_TYPE device_type, char * buffer, bool name_only) {
    const uint8_t size    = 128;
    bool          enabled = false;
    uint8_t       device_id;
    uint8_t       product_id;
    char *        version;
    const char *  device_desc_p;

    if (device_type == EMS_DEVICE_TYPE_THERMOSTAT) {
        enabled       = ems_getThermostatEnabled();
        device_id     = EMS_Thermostat.device_id;
        product_id    = EMS_Thermostat.product_id;
        device_desc_p = EMS_Thermostat.device_desc_p;
        version       = EMS_Thermostat.version;
    } else if (device_type == EMS_DEVICE_TYPE_BOILER) {
        enabled       = ems_getBoilerEnabled();
        device_id     = EMS_Boiler.device_id;
        product_id    = EMS_Boiler.product_id;
        device_desc_p = EMS_Boiler.device_desc_p;
        version       = EMS_Boiler.version;
    } else if (device_type == EMS_DEVICE_TYPE_SOLAR) {
        enabled       = ems_getSolarModuleEnabled();
        device_id     = EMS_SolarModule.device_id;
        product_id    = EMS_SolarModule.product_id;
        device_desc_p = EMS_SolarModule.device_desc_p;
        version       = EMS_SolarModule.version;
    } else if (device_type == EMS_DEVICE_TYPE_HEATPUMP) {
        enabled       = ems_getHeatPumpEnabled();
        device_id     = EMS_HeatPump.device_id;
        product_id    = EMS_HeatPump.product_id;
        device_desc_p = EMS_HeatPump.device_desc_p;
        version       = EMS_HeatPump.version;
    }

    if (!enabled) {
        strlcpy(buffer, "<not enabled>", size);
        return buffer;
    }

    // assume at this point we have a known device.
    // get device description
    if (device_desc_p == nullptr) {
        strlcpy(buffer, EMS_MODELTYPE_UNKNOWN_STRING, size);
    } else {
        strlcpy(buffer, device_desc_p, size);
    }

    if (name_only) {
        return buffer; // only interested in the model name
    }

    strlcat(buffer, " (DeviceID:0x", size);
    char tmp[6] = {0};
    strlcat(buffer, _hextoa(device_id, tmp), size);
    strlcat(buffer, " ProductID:", size);
    strlcat(buffer, itoa(product_id, tmp, 10), size);
    strlcat(buffer, " Version:", size);
    strlcat(buffer, version, size);
    strlcat(buffer, ")", size);

    return buffer;
}

/**
 * Find the versions of our connected devices
 */
void ems_scanDevices() {
    myDebug_P(PSTR("Started scanning the EMS bus for known devices"));

    std::list<uint8_t> Device_Ids; // create a new list

    Device_Ids.push_back(EMS_ID_BOILER); // UBAMaster/Boilers - 0x08
    Device_Ids.push_back(EMS_ID_HP);     // HeatPump - 0x38
    Device_Ids.push_back(EMS_ID_SM);     // Solar Module - 0x30
    Device_Ids.push_back(0x09);          // Controllers - 0x09
    Device_Ids.push_back(0x02);          // Connect - 0x02
    Device_Ids.push_back(0x48);          // Gateway - 0x48
    Device_Ids.push_back(0x20);          // Mixing Devices - 0x20, 0x21
    Device_Ids.push_back(0x21);          // Mixing Devices - 0x20, 0x21
    Device_Ids.push_back(0x10);          // Thermostats - 0x10, 0x17, 0x18
    Device_Ids.push_back(0x17);          // Thermostats - 0x10, 0x17, 0x18
    Device_Ids.push_back(0x18);          // Thermostats - 0x10, 0x17, 0x18

    // remove duplicates and reserved IDs (like our own device)
    Device_Ids.sort();
    // Device_Ids.unique();

    // send the read command with Version command
    for (uint8_t device_id : Device_Ids) {
        ems_doReadCommand(EMS_TYPE_Version, device_id);
    }
}

/**
 * print out contents of the device list that was captured
 */
void ems_printDevices() {
    char s[100];
    char buffer[16] = {0};

    strlcpy(s, "These device IDs are on the EMS Bus:", sizeof(s));
    strlcat(s, COLOR_BOLD_ON, sizeof(s));

    for (uint8_t data_byte = 0; data_byte < EMS_SYS_DEVICEMAP_LENGTH; data_byte++) {
        uint8_t byte = EMS_Sys_Status.emsDeviceMap[data_byte];
        if (byte) {
            // go through all bits
            for (uint8_t bit = 0; bit < 8; bit++) {
                if (byte & 0x01) {
                    uint8_t device_id = ((data_byte + 1) * 8) + bit;
                    if (device_id != EMS_ID_ME) {
                        strlcat(s, " 0x", sizeof(s));
                        strlcat(s, _hextoa(device_id, buffer), sizeof(s));
                    }
                }
                byte = byte >> 1;
            }
        }
    }

    strlcat(s, COLOR_BOLD_OFF, sizeof(s));
    myDebug_P(PSTR("")); // newline
    myDebug(s);

    // print out the ones we recognized
    if (!Devices.empty()) {
        bool have_unknowns = false;
        char device_string[100];
        myDebug_P(PSTR("and %d were recognized by EMS-ESP as:"), Devices.size());
        for (std::list<_Detected_Device>::iterator it = Devices.begin(); it != Devices.end(); ++it) {
            if ((it)->known) {
                strlcpy(device_string, (it)->device_desc_p, sizeof(device_string));
            } else {
                strlcpy(device_string, EMS_MODELTYPE_UNKNOWN_STRING, sizeof(device_string)); // Unknown
                have_unknowns = true;
            }

            myDebug_P(PSTR(" %s%s%s (DeviceID:0x%02X ProductID:%d Version:%s)"),
                      COLOR_BOLD_ON,
                      device_string,
                      COLOR_BOLD_OFF,
                      (it)->device_id,
                      (it)->product_id,
                      (it)->version);
        }

        myDebug_P(PSTR("")); // newline

        if (have_unknowns) {
            myDebug_P(
                PSTR("You have a device is that is not yet known by EMS-ESP. Please report this as a GitHub issue so we can expand the EMS device library."));
        }
    } else {
        myDebug_P(PSTR("No were devices recognized. This may be because Tx is disabled or failing."));
    }

    myDebug_P(PSTR("")); // newline
}

/**
 * Send a raw telegram to the bus
 * telegram is a string of hex values
 */
void ems_sendRawTelegram(char * telegram) {
    if (EMS_Sys_Status.emsTxDisabled) {
        if (ems_getLogging() != EMS_SYS_LOGGING_NONE) {
            myDebug_P(PSTR("in Listen Mode. All Tx is disabled."));
        }
        return;
    }

    uint8_t count = 0;
    char *  p;
    char    value[10] = {0};

    _EMS_TxTelegram EMS_TxTelegram = EMS_TX_TELEGRAM_NEW; // create new Tx
    EMS_TxTelegram.timestamp       = millis();            // set timestamp
    EMS_Sys_Status.txRetryCount    = 0;                   // reset retry counter

    // get first value, which should be the src
    if ((p = strtok(telegram, " ,"))) { // delimiter
        strlcpy(value, p, sizeof(value));
        EMS_TxTelegram.data[0] = (uint8_t)strtol(value, 0, 16);
    }
    // and interate until end
    while (p != 0) {
        if ((p = strtok(nullptr, " ,"))) {
            strlcpy(value, p, sizeof(value));
            uint8_t val                  = (uint8_t)strtol(value, 0, 16);
            EMS_TxTelegram.data[++count] = val;
            if (count == 1) {
                EMS_TxTelegram.dest = val;
            } else if (count == 2) {
                EMS_TxTelegram.type = val;
            } else if (count == 3) {
                EMS_TxTelegram.offset = val;
            }
        }
    }

    if (count == 0) {
        return; // nothing to send
    }

    EMS_TxTelegram.length        = count + 2;
    EMS_TxTelegram.type_validate = EMS_ID_NONE;
    EMS_TxTelegram.action        = EMS_TX_TELEGRAM_RAW;

    // add to Tx queue. Assume it's not full.
    EMS_TxQueue.push(EMS_TxTelegram);
}

/**
 * Set the temperature of the thermostat
 * hc_num is 1 to 4
 * temptype 0 = normal, 1=night temp, 2=day temp, 3=holiday temp
 */
void ems_setThermostatTemp(float temperature, uint8_t hc_num, uint8_t temptype) {
    if (!ems_getThermostatEnabled()) {
        myDebug_P(PSTR("Thermostat not online."));
        return;
    }

    if (!EMS_Thermostat.write_supported) {
        myDebug_P(PSTR("Write not supported yet for this Thermostat model"));
        return;
    }

    if (hc_num < 1 || hc_num > EMS_THERMOSTAT_MAXHC) {
        myDebug_P(PSTR("Invalid HC number"));
        return;
    }

    _EMS_TxTelegram EMS_TxTelegram = EMS_TX_TELEGRAM_NEW; // create new Tx
    EMS_TxTelegram.timestamp       = millis();            // set timestamp
    EMS_Sys_Status.txRetryCount    = 0;                   // reset retry counter

    uint8_t model     = ems_getThermostatModel();
    uint8_t device_id = EMS_Thermostat.device_id;

    EMS_TxTelegram.action = EMS_TX_TELEGRAM_WRITE;
    EMS_TxTelegram.dest   = device_id;

    char s[10] = {0};
    myDebug_P(PSTR("Setting new thermostat temperature to %s for heating circuit %d type %d (0=auto,1=night,2=day,3=holiday)"),
              _float_to_char(s, temperature),
              hc_num,
              temptype);

    if (model == EMS_DEVICE_FLAG_RC20) {
        EMS_TxTelegram.type               = EMS_TYPE_RC20Set;
        EMS_TxTelegram.offset             = EMS_OFFSET_RC20Set_temp;
        EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RC20StatusMessage;
        EMS_TxTelegram.type_validate      = EMS_TxTelegram.type;

    } else if (model == EMS_DEVICE_FLAG_RC10) {
        EMS_TxTelegram.type               = EMS_TYPE_RC10Set;
        EMS_TxTelegram.offset             = EMS_OFFSET_RC10Set_temp;
        EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RC10StatusMessage;
        EMS_TxTelegram.type_validate      = EMS_TxTelegram.type;

    } else if (model == EMS_DEVICE_FLAG_RC30) {
        EMS_TxTelegram.type               = EMS_TYPE_RC30Set;
        EMS_TxTelegram.offset             = EMS_OFFSET_RC30Set_temp;
        EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RC30StatusMessage;
        EMS_TxTelegram.type_validate      = EMS_TxTelegram.type;

    } else if (model == EMS_DEVICE_FLAG_RC300) {
        // check mode to determine offset
        if (EMS_Thermostat.hc[hc_num - 1].mode == 1) {        // auto
            EMS_TxTelegram.offset = 0x08;                     // auto offset
        } else if (EMS_Thermostat.hc[hc_num - 1].mode == 0) { // manuaL
            EMS_TxTelegram.offset = 0x0A;                     // manual offset
        }

        if (hc_num == 1) {
            EMS_TxTelegram.type               = EMS_TYPE_RCPLUSSet; // for 3000 and 1010, e.g. 0B 10 FF (0A | 08) 01 89 2B
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RCPLUSStatusMessage_HC1;
        } else if (hc_num == 2) {
            EMS_TxTelegram.type               = EMS_TYPE_RCPLUSSet + 1;
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RCPLUSStatusMessage_HC2;
        } else if (hc_num == 3) {
            EMS_TxTelegram.type               = EMS_TYPE_RCPLUSSet + 2;
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RCPLUSStatusMessage_HC3;
        } else if (hc_num == 4) {
            EMS_TxTelegram.type               = EMS_TYPE_RCPLUSSet + 3;
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RCPLUSStatusMessage_HC4;
        }

        EMS_TxTelegram.type_validate = EMS_ID_NONE; // validate by reading from a different telegram

    } else if (model == EMS_DEVICE_FLAG_RC35) {
        switch (temptype) {
        case 1: // change the night temp
            EMS_TxTelegram.offset = EMS_OFFSET_RC35Set_temp_night;
            break;
        case 2: // change the day temp
            EMS_TxTelegram.offset = EMS_OFFSET_RC35Set_temp_day;
            break;
        case 3: // change the holiday temp
            EMS_TxTelegram.offset = EMS_OFFSET_RC35Set_temp_holiday;
            break;
        default:
        case 0: // automatic selection, if no type is defined, we use the standard code
            if (EMS_Thermostat.hc[hc_num - 1].day_mode == 0) {
                EMS_TxTelegram.offset = EMS_OFFSET_RC35Set_temp_night;
            } else if (EMS_Thermostat.hc[hc_num - 1].day_mode == 1) {
                EMS_TxTelegram.offset = EMS_OFFSET_RC35Set_temp_day;
            }
            break;
        }

        if (hc_num == 1) {
            EMS_TxTelegram.type               = EMS_TYPE_RC35Set_HC1;
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RC35StatusMessage_HC1;
        } else if (hc_num == 2) {
            EMS_TxTelegram.type               = EMS_TYPE_RC35Set_HC2;
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RC35StatusMessage_HC2;
        } else if (hc_num == 3) {
            EMS_TxTelegram.type               = EMS_TYPE_RC35Set_HC3;
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RC35StatusMessage_HC3;
        } else if (hc_num == 4) {
            EMS_TxTelegram.type               = EMS_TYPE_RC35Set_HC4;
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RC35StatusMessage_HC4;
        }
        EMS_TxTelegram.type_validate = EMS_TxTelegram.type;
    }

    EMS_TxTelegram.length           = EMS_MIN_TELEGRAM_LENGTH;
    EMS_TxTelegram.dataValue        = (uint8_t)((float)temperature * (float)2); // value * 2
    EMS_TxTelegram.comparisonOffset = EMS_TxTelegram.offset;
    EMS_TxTelegram.comparisonValue  = EMS_TxTelegram.dataValue;

    EMS_TxQueue.push(EMS_TxTelegram);
}

/**
 * Set the thermostat working mode
 *  0xA8 on a RC20 and 0xA7 on RC30
 *  0x01B9 for EMS+ 300/1000/3000, Auto=0xFF Manual=0x00. See https://github.com/proddy/EMS-ESP/wiki/RC3xx-Thermostats
 *  hc_num is 1 to 4
 */
void ems_setThermostatMode(uint8_t mode, uint8_t hc_num) {
    if (!ems_getThermostatEnabled()) {
        myDebug_P(PSTR("Thermostat not online."));
        return;
    }

    if (!EMS_Thermostat.write_supported) {
        myDebug_P(PSTR("Write not supported for this model Thermostat"));
        return;
    }

    if (hc_num < 1 || hc_num > EMS_THERMOSTAT_MAXHC) {
        myDebug_P(PSTR("Invalid HC number"));
        return;
    }

    uint8_t model     = ems_getThermostatModel();
    uint8_t device_id = EMS_Thermostat.device_id;
    uint8_t set_mode;

    // RC300/1000/3000 have different settings
    if (model == EMS_DEVICE_FLAG_RC300) {
        if (mode == 1) {
            set_mode = 0; // manual
        } else {
            set_mode = 0xFF; // auto
        }
    } else {
        set_mode = mode;
    }

    // 0=off, 1=manual, 2=auto
    if (mode == 0) {
        myDebug_P(PSTR("Setting thermostat mode to off for heating circuit %d"), hc_num);
    } else if (set_mode == 1) {
        myDebug_P(PSTR("Setting thermostat mode to manual for heating circuit %d"), hc_num);
    } else if (set_mode == 2) {
        myDebug_P(PSTR("Setting thermostat mode to auto for heating circuit %d"), hc_num);
    }

    _EMS_TxTelegram EMS_TxTelegram = EMS_TX_TELEGRAM_NEW; // create new Tx
    EMS_TxTelegram.timestamp       = millis();            // set timestamp
    EMS_Sys_Status.txRetryCount    = 0;                   // reset retry counter

    EMS_TxTelegram.action    = EMS_TX_TELEGRAM_WRITE;
    EMS_TxTelegram.dest      = device_id;
    EMS_TxTelegram.length    = EMS_MIN_TELEGRAM_LENGTH;
    EMS_TxTelegram.dataValue = set_mode;

    // handle different thermostat types
    if (model == EMS_DEVICE_FLAG_RC20) {
        EMS_TxTelegram.type               = EMS_TYPE_RC20Set;
        EMS_TxTelegram.offset             = EMS_OFFSET_RC20Set_mode;
        EMS_TxTelegram.type_validate      = EMS_TYPE_RC20Set;
        EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RC20StatusMessage;

    } else if (model == EMS_DEVICE_FLAG_RC30) {
        EMS_TxTelegram.type               = EMS_TYPE_RC30Set;
        EMS_TxTelegram.offset             = EMS_OFFSET_RC30Set_mode;
        EMS_TxTelegram.type_validate      = EMS_TYPE_RC30Set;
        EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RC30StatusMessage;

    } else if (model == EMS_DEVICE_FLAG_RC35) {
        if (hc_num == 1) {
            EMS_TxTelegram.type               = EMS_TYPE_RC35Set_HC1;
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RC35StatusMessage_HC1;
        } else if (hc_num == 2) {
            EMS_TxTelegram.type               = EMS_TYPE_RC35Set_HC2;
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RC35StatusMessage_HC2;
        } else if (hc_num == 3) {
            EMS_TxTelegram.type               = EMS_TYPE_RC35Set_HC3;
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RC35StatusMessage_HC3;
        } else if (hc_num == 4) {
            EMS_TxTelegram.type               = EMS_TYPE_RC35Set_HC4;
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RC35StatusMessage_HC4;
        }
        EMS_TxTelegram.offset        = EMS_OFFSET_RC35Set_mode;
        EMS_TxTelegram.type_validate = EMS_TxTelegram.type;

    } else if (model == EMS_DEVICE_FLAG_RC300) {
        EMS_TxTelegram.offset = EMS_OFFSET_RCPLUSSet_mode;

        if (hc_num == 1) {
            EMS_TxTelegram.type               = EMS_TYPE_RCPLUSSet;
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RCPLUSStatusMessage_HC1;
        } else if (hc_num == 2) {
            EMS_TxTelegram.type               = EMS_TYPE_RCPLUSSet + 1;
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RCPLUSStatusMessage_HC2;
        } else if (hc_num == 3) {
            EMS_TxTelegram.type               = EMS_TYPE_RCPLUSSet + 2;
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RCPLUSStatusMessage_HC3;
        } else if (hc_num == 4) {
            EMS_TxTelegram.type               = EMS_TYPE_RCPLUSSet + 3;
            EMS_TxTelegram.comparisonPostRead = EMS_TYPE_RCPLUSStatusMessage_HC4;
        }

        EMS_TxTelegram.type_validate = EMS_ID_NONE; // don't validate after the write
    }

    EMS_TxTelegram.comparisonOffset = EMS_TxTelegram.offset;
    EMS_TxTelegram.comparisonValue  = EMS_TxTelegram.dataValue;

    EMS_TxQueue.push(EMS_TxTelegram);
}

/**
 * Set the warm water temperature 0x33
 */
void ems_setWarmWaterTemp(uint8_t temperature) {
    // check for invalid temp values
    if ((temperature < 30) || (temperature > EMS_BOILER_TAPWATER_TEMPERATURE_MAX)) {
        return;
    }

    myDebug_P(PSTR("Setting boiler warm water temperature to %d C"), temperature);

    _EMS_TxTelegram EMS_TxTelegram = EMS_TX_TELEGRAM_NEW; // create new Tx
    EMS_TxTelegram.timestamp       = millis();            // set timestamp
    EMS_Sys_Status.txRetryCount    = 0;                   // reset retry counter

    EMS_TxTelegram.action    = EMS_TX_TELEGRAM_WRITE;
    EMS_TxTelegram.dest      = EMS_Boiler.device_id;
    EMS_TxTelegram.type      = EMS_TYPE_UBAParameterWW;
    EMS_TxTelegram.offset    = EMS_OFFSET_UBAParameterWW_wwtemp;
    EMS_TxTelegram.length    = EMS_MIN_TELEGRAM_LENGTH;
    EMS_TxTelegram.dataValue = temperature; // int value to compare against

    EMS_TxTelegram.type_validate      = EMS_TYPE_UBAParameterWW; // validate
    EMS_TxTelegram.comparisonOffset   = EMS_OFFSET_UBAParameterWW_wwtemp;
    EMS_TxTelegram.comparisonValue    = temperature;
    EMS_TxTelegram.comparisonPostRead = EMS_TYPE_UBAParameterWW;

    EMS_TxQueue.push(EMS_TxTelegram);
}

/**
 * Set the boiler flow temp
 */
void ems_setFlowTemp(uint8_t temperature) {
    myDebug_P(PSTR("Setting boiler flow temperature to %d C"), temperature);

    _EMS_TxTelegram EMS_TxTelegram = EMS_TX_TELEGRAM_NEW; // create new Tx
    EMS_TxTelegram.timestamp       = millis();            // set timestamp
    EMS_Sys_Status.txRetryCount    = 0;                   // reset retry counter

    EMS_TxTelegram.action    = EMS_TX_TELEGRAM_WRITE;
    EMS_TxTelegram.dest      = EMS_Boiler.device_id;
    EMS_TxTelegram.type      = EMS_TYPE_UBASetPoints;
    EMS_TxTelegram.offset    = EMS_OFFSET_UBASetPoints_flowtemp;
    EMS_TxTelegram.length    = EMS_MIN_TELEGRAM_LENGTH;
    EMS_TxTelegram.dataValue = temperature; // int value to compare against

    EMS_TxTelegram.type_validate      = EMS_TYPE_UBASetPoints; // validate
    EMS_TxTelegram.comparisonOffset   = EMS_OFFSET_UBASetPoints_flowtemp;
    EMS_TxTelegram.comparisonValue    = temperature;
    EMS_TxTelegram.comparisonPostRead = EMS_TYPE_UBASetPoints;

    EMS_TxQueue.push(EMS_TxTelegram);
}

/**
 * Set the warm water mode to comfort to Eco/Comfort
 * 1 = Hot, 2 = Eco, 3 = Intelligent
 */
void ems_setWarmWaterModeComfort(uint8_t comfort) {
    _EMS_TxTelegram EMS_TxTelegram = EMS_TX_TELEGRAM_NEW; // create new Tx
    EMS_TxTelegram.timestamp       = millis();            // set timestamp
    EMS_Sys_Status.txRetryCount    = 0;                   // reset retry counter

    if (comfort == 1) {
        myDebug_P(PSTR("Setting boiler warm water comfort mode to Hot"));
        EMS_TxTelegram.dataValue = EMS_VALUE_UBAParameterWW_wwComfort_Hot;
    } else if (comfort == 2) {
        myDebug_P(PSTR("Setting boiler warm water comfort mode to Eco"));
        EMS_TxTelegram.dataValue = EMS_VALUE_UBAParameterWW_wwComfort_Eco;
    } else if (comfort == 3) {
        myDebug_P(PSTR("Setting boiler warm water comfort mode to Intelligent"));
        EMS_TxTelegram.dataValue = EMS_VALUE_UBAParameterWW_wwComfort_Intelligent;
    } else {
        return; // invalid comfort value
    }

    EMS_TxTelegram.action        = EMS_TX_TELEGRAM_WRITE;
    EMS_TxTelegram.dest          = EMS_Boiler.device_id;
    EMS_TxTelegram.type          = EMS_TYPE_UBAParameterWW;
    EMS_TxTelegram.offset        = EMS_OFFSET_UBAParameterWW_wwComfort;
    EMS_TxTelegram.length        = EMS_MIN_TELEGRAM_LENGTH;
    EMS_TxTelegram.type_validate = EMS_ID_NONE; // don't validate

    EMS_TxQueue.push(EMS_TxTelegram);
}

/**
 * Activate / De-activate the Warm Water 0x33
 * true = on, false = off
 */
void ems_setWarmWaterActivated(bool activated) {
    myDebug_P(PSTR("Setting boiler warm water %s"), activated ? "on" : "off");

    _EMS_TxTelegram EMS_TxTelegram = EMS_TX_TELEGRAM_NEW; // create new Tx
    EMS_TxTelegram.timestamp       = millis();            // set timestamp
    EMS_Sys_Status.txRetryCount    = 0;                   // reset retry counter

    EMS_TxTelegram.action        = EMS_TX_TELEGRAM_WRITE;
    EMS_TxTelegram.dest          = EMS_Boiler.device_id;
    EMS_TxTelegram.type          = EMS_TYPE_UBAParameterWW;
    EMS_TxTelegram.offset        = EMS_OFFSET_UBAParameterWW_wwactivated;
    EMS_TxTelegram.length        = EMS_MIN_TELEGRAM_LENGTH;
    EMS_TxTelegram.type_validate = EMS_ID_NONE;               // don't validate
    EMS_TxTelegram.dataValue     = (activated ? 0xFF : 0x00); // 0xFF is on, 0x00 is off

    EMS_TxQueue.push(EMS_TxTelegram);
}

/**
 * Activate / De-activate the Warm Tap Water
 * true = on, false = off
 * Using the type 0x1D to put the boiler into Test mode. This may be shown on the boiler with a flashing 'T'
 */
void ems_setWarmTapWaterActivated(bool activated) {
    myDebug_P(PSTR("Setting boiler warm tap water %s"), activated ? "on" : "off");

    _EMS_TxTelegram EMS_TxTelegram = EMS_TX_TELEGRAM_NEW; // create new Tx
    EMS_TxTelegram.timestamp       = millis();            // set timestamp
    EMS_Sys_Status.txRetryCount    = 0;                   // reset retry counter

    // clear Tx to make sure all data is set to 0x00
    for (int i = 0; (i < EMS_MAX_TELEGRAM_LENGTH); i++) {
        EMS_TxTelegram.data[i] = 0x00;
    }

    EMS_TxTelegram.action = EMS_TX_TELEGRAM_WRITE;
    EMS_TxTelegram.dest   = EMS_Boiler.device_id;
    EMS_TxTelegram.type   = EMS_TYPE_UBAFunctionTest;
    EMS_TxTelegram.offset = 0;

    EMS_TxTelegram.type_validate      = EMS_TxTelegram.type;
    EMS_TxTelegram.comparisonOffset   = 0;                   // 1st byte
    EMS_TxTelegram.comparisonValue    = (activated ? 0 : 1); // value is 1 if in Test mode (not activated)
    EMS_TxTelegram.comparisonPostRead = EMS_TxTelegram.type;

    // create header
    EMS_TxTelegram.data[0] = EMS_ID_ME;             // src
    EMS_TxTelegram.data[1] = EMS_TxTelegram.dest;   // dest
    EMS_TxTelegram.data[2] = EMS_TxTelegram.type;   // type
    EMS_TxTelegram.data[3] = EMS_TxTelegram.offset; // offset

    // we use the special test mode 0x1D for this. Setting the first data to 5A puts the system into test mode and
    // a setting of 0x00 puts it back into normal operarting mode
    // when in test mode we're able to mess around with the 3-way valve settings
    if (!activated) {
        // on
        EMS_TxTelegram.data[4] = 0x5A; // test mode on
        EMS_TxTelegram.data[5] = 0x00; // burner output 0%
        EMS_TxTelegram.data[7] = 0x64; // boiler pump capacity 100%
        EMS_TxTelegram.data[8] = 0xFF; // 3-way valve hot water only
        EMS_TxTelegram.length  = 22;   // 17 bytes of data including header and CRC. We send all zeros just to be sure.
    } else {
        // get out of test mode
        // telegram: 0B 08 1D 00 00
        EMS_TxTelegram.data[4] = 0x00; // test mode off
        EMS_TxTelegram.length  = EMS_MIN_TELEGRAM_LENGTH;
    }

    EMS_TxQueue.push(EMS_TxTelegram); // add to queue
}

/**
 * Start up sequence for UBA Master, hopefully to initialize a handshake
 * Still experimental and not used yet!
 */
void ems_startupTelegrams() {
    if ((ems_getTxDisabled()) || (!ems_getBusConnected())) {
        myDebug_P(PSTR("Unable to send startup sequence when in listen mode or the bus is disabled"));
    }

    myDebug_P(PSTR("Sending startup sequence..."));
    char s[20] = {0};

    // Write type 0x1D to get out of function test mode
    snprintf(s, sizeof(s), "%02X %02X 1D 00 00", EMS_ID_ME, EMS_Boiler.device_id);
    ems_sendRawTelegram(s);

    // Read type 0x01
    snprintf(s, sizeof(s), "%02X %02X 01 00 1B", EMS_ID_ME, EMS_Boiler.device_id | 0x80);
    ems_sendRawTelegram(s);
}

/**
 * Test parsing of telgrams by injecting fake telegrams and simulating the response
 */
void ems_testTelegram(uint8_t test_num) {
#ifdef TESTS
    if ((test_num == 0) || (test_num > _TEST_DATA_max)) {
        myDebug_P(PSTR("Invalid test. Pick between 1 and %d"), _TEST_DATA_max);
        return;
    }

    // stop all Tx
    if (!EMS_TxQueue.isEmpty()) {
        EMS_TxQueue.clear();
        EMS_Sys_Status.emsTxStatus = EMS_TX_STATUS_IDLE;
    }

    static uint8_t * telegram = (uint8_t *)malloc(EMS_MAX_TELEGRAM_LENGTH); // warning, memory is never set free so use only for debugging

    char telegram_string[200];
    strlcpy(telegram_string, TEST_DATA[test_num - 1], sizeof(telegram_string));

    uint8_t length = 0;
    char *  p;
    char    value[10] = {0};

    // get first value, which should be the src
    if ((p = strtok(telegram_string, " ,"))) {
        strlcpy(value, p, sizeof(value));
        telegram[0] = (uint8_t)strtol(value, 0, 16);
    }

    // and interate until end
    while (p != 0) {
        if ((p = strtok(nullptr, " ,"))) {
            strlcpy(value, p, sizeof(value));
            uint8_t val        = (uint8_t)strtol(value, 0, 16);
            telegram[++length] = val;
        }
    }

    length++;                                                // this is the total amount of bytes
    telegram[length] = _crcCalculator(telegram, length + 1); // add the CRC

    myDebug_P(PSTR("[TEST %d] Injecting telegram %s"), test_num, TEST_DATA[test_num - 1]);

    // go an parse it
    ems_parseTelegram(telegram, length + 1); // include CRC in length
#else
    myDebug_P(PSTR("Firmware not compiled with test data. Use -DTESTS"));
#endif
}

/**
 * Recognized EMS types and the functions they call to process the telegrams
 */
const _EMS_Type EMS_Types[] = {

    // common
    {EMS_DEVICE_UPDATE_FLAG_NONE, EMS_TYPE_Version, "Version", _process_Version},
    {EMS_DEVICE_UPDATE_FLAG_NONE, EMS_TYPE_UBADevices, "UBADevices", _process_UBADevices},
    {EMS_DEVICE_UPDATE_FLAG_NONE, EMS_TYPE_RCTime, "RCTime", _process_RCTime},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RCOutdoorTempMessage, "RCOutdoorTempMessage", _process_RCOutdoorTempMessage},

    // UBA/Boiler
    {EMS_DEVICE_UPDATE_FLAG_BOILER, EMS_TYPE_UBAMonitorFast, "UBAMonitorFast", _process_UBAMonitorFast},
    {EMS_DEVICE_UPDATE_FLAG_BOILER, EMS_TYPE_UBAMonitorSlow, "UBAMonitorSlow", _process_UBAMonitorSlow},
    {EMS_DEVICE_UPDATE_FLAG_BOILER, EMS_TYPE_UBAMonitorWWMessage, "UBAMonitorWWMessage", _process_UBAMonitorWWMessage},
    {EMS_DEVICE_UPDATE_FLAG_BOILER, EMS_TYPE_UBAParameterWW, "UBAParameterWW", _process_UBAParameterWW},
    {EMS_DEVICE_UPDATE_FLAG_BOILER, EMS_TYPE_UBATotalUptimeMessage, "UBATotalUptimeMessage", _process_UBATotalUptimeMessage},
    {EMS_DEVICE_UPDATE_FLAG_BOILER, EMS_TYPE_UBAParametersMessage, "UBAParametersMessage", _process_UBAParametersMessage},
    {EMS_DEVICE_UPDATE_FLAG_BOILER, EMS_TYPE_UBASetPoints, "UBASetPoints", _process_SetPoints},

    // UBA/Boiler EMS+
    {EMS_DEVICE_UPDATE_FLAG_BOILER, EMS_TYPE_UBAOutdoorTemp, "UBAOutdoorTemp", _process_UBAOutdoorTemp},
    {EMS_DEVICE_UPDATE_FLAG_BOILER, EMS_TYPE_UBAMonitorFast2, "UBAMonitorFast2", _process_UBAMonitorFast2},
    {EMS_DEVICE_UPDATE_FLAG_BOILER, EMS_TYPE_UBAMonitorSlow2, "UBAMonitorSlow2", _process_UBAMonitorSlow2},

    // Solar Module devices
    {EMS_DEVICE_UPDATE_FLAG_SOLAR, EMS_TYPE_SM10Monitor, "SM10Monitor", _process_SM10Monitor},
    {EMS_DEVICE_UPDATE_FLAG_SOLAR, EMS_TYPE_SM100Monitor, "SM100Monitor", _process_SM100Monitor},
    {EMS_DEVICE_UPDATE_FLAG_SOLAR, EMS_TYPE_SM100Status, "SM100Status", _process_SM100Status},
    {EMS_DEVICE_UPDATE_FLAG_SOLAR, EMS_TYPE_SM100Status2, "SM100Status2", _process_SM100Status2},
    {EMS_DEVICE_UPDATE_FLAG_SOLAR, EMS_TYPE_SM100Energy, "SM100Energy", _process_SM100Energy},
    {EMS_DEVICE_UPDATE_FLAG_SOLAR, EMS_TYPE_ISM1StatusMessage, "ISM1StatusMessage", _process_ISM1StatusMessage},
    {EMS_DEVICE_UPDATE_FLAG_SOLAR, EMS_TYPE_ISM1Set, "ISM1Set", _process_ISM1Set},

    // heatpumps
    {EMS_DEVICE_UPDATE_FLAG_HEATPUMP, EMS_TYPE_HPMonitor1, "HeatPumpMonitor1", _process_HPMonitor1},
    {EMS_DEVICE_UPDATE_FLAG_HEATPUMP, EMS_TYPE_HPMonitor2, "HeatPumpMonitor2", _process_HPMonitor2},

    // RC10
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RC10Set, "RC10Set", _process_RC10Set},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RC10StatusMessage, "RC10StatusMessage", _process_RC10StatusMessage},

    // RC20 and RC20RF
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RC20Set, "RC20Set", _process_RC20Set},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RC20StatusMessage, "RC20StatusMessage", _process_RC20StatusMessage},

    // RC30
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RC30Set, "RC30Set", _process_RC30Set},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RC30StatusMessage, "RC30StatusMessage", _process_RC30StatusMessage},

    // RC35 and ES71
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RC35Set_HC1, "RC35Set_HC1", _process_RC35Set},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RC35StatusMessage_HC1, "RC35StatusMessage_HC1", _process_RC35StatusMessage},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RC35Set_HC2, "RC35Set_HC2", _process_RC35Set},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RC35StatusMessage_HC2, "RC35StatusMessage_HC2", _process_RC35StatusMessage},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RC35Set_HC3, "RC35Set_HC2", _process_RC35Set},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RC35StatusMessage_HC3, "RC35StatusMessage_HC3", _process_RC35StatusMessage},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RC35Set_HC4, "RC35Set_HC4", _process_RC35Set},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RC35StatusMessage_HC4, "RC35StatusMessage_HC4", _process_RC35StatusMessage},

    // Easy
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_EasyStatusMessage, "EasyStatusMessage", _process_EasyStatusMessage},

    // Nefit 1010, RC300, RC310 (EMS Plus)
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RCPLUSStatusMessage_HC1, "RCPLUSStatusMessage_HC1", _process_RCPLUSStatusMessage},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RCPLUSStatusMessage_HC2, "RCPLUSStatusMessage_HC2", _process_RCPLUSStatusMessage},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RCPLUSStatusMessage_HC3, "RCPLUSStatusMessage_HC3", _process_RCPLUSStatusMessage},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RCPLUSStatusMessage_HC4, "RCPLUSStatusMessage_HC4", _process_RCPLUSStatusMessage},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RCPLUSSet, "RCPLUSSetMessage", _process_RCPLUSSetMessage},
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_RCPLUSStatusMode, "RCPLUSStatusMode", _process_RCPLUSStatusMode},

    // Junkers FR10
    {EMS_DEVICE_UPDATE_FLAG_THERMOSTAT, EMS_TYPE_JunkersStatusMessage, "JunkersStatusMessage", _process_JunkersStatusMessage},

    // Mixing devices
    {EMS_DEVICE_UPDATE_FLAG_MIXING, EMS_TYPE_MMPLUSStatusMessage_HC1, "MMPLUSStatusMessage_HC1", _process_MMPLUSStatusMessage},
    {EMS_DEVICE_UPDATE_FLAG_MIXING, EMS_TYPE_MMPLUSStatusMessage_HC2, "MMPLUSStatusMessage_HC2", _process_MMPLUSStatusMessage}

};

// calculate sizes of arrays at compile time
uint8_t _EMS_Types_max = ArraySize(EMS_Types);

/**
 * Find the pointer to the EMS_Types array for a given type ID
 * or -1 if not found
 */
int8_t _ems_findType(uint16_t type) {
    uint8_t i         = 0;
    bool    typeFound = false;
    // scan through known ID types
    while (i < _EMS_Types_max) {
        if (EMS_Types[i].type == type) {
            typeFound = true; // we have a match
            break;
        }
        i++;
    }

    return (typeFound ? i : -1);
}

/**
 * print detailed telegram
 * and then call its callback if there is one defined
 */
void _ems_processTelegram(_EMS_RxTelegram * EMS_RxTelegram) {
    // print out the telegram for verbose mode
    if (EMS_Sys_Status.emsLogging >= EMS_SYS_LOGGING_THERMOSTAT) {
        _printMessage(EMS_RxTelegram);
    }

    // ignore telegrams that don't have any data
    if (EMS_RxTelegram->data_length == 0) {
        return;
    }

    // we're only interested in broadcast messages (dest is 0x00) or ones for us (dest is 0x0B)
    uint8_t dest = EMS_RxTelegram->dest;
    if ((dest != EMS_ID_NONE) && (dest != EMS_ID_ME)) {
        return;
    }

    // see if we recognize the type first by scanning our known EMS types list
    uint16_t type = EMS_RxTelegram->type;
    int8_t   i    = _ems_findType(type);
    if (i == -1) {
        return; // not found
    }

    // if it's a common type (across ems devices) or something specifically for us process it.
    // dest will be EMS_ID_NONE and offset 0x00 for a broadcast message
    if ((EMS_Types[i].processType_cb) != nullptr) {
        // print non-verbose message
        if (EMS_Sys_Status.emsLogging == EMS_SYS_LOGGING_BASIC) {
            myDebug_P(PSTR("<--- %s(0x%02X)"), EMS_Types[i].typeString, type);
        }
        // call callback function to process the telegram
        (void)EMS_Types[i].processType_cb(EMS_RxTelegram);

        // see if we need to flag something has changed
        ems_Device_add_flags(EMS_Types[i].device_flag);
    }

    EMS_Sys_Status.emsTxStatus = EMS_TX_STATUS_IDLE;
}

/**
 * deciphers the telegram packet, which has already been checked for valid CRC and has a complete header
 * length is only data bytes, excluding the BRK
 * We only remove from the Tx queue if the read or write was successful
 */
void _processType(_EMS_RxTelegram * EMS_RxTelegram) {
    uint8_t * telegram = EMS_RxTelegram->telegram;

    // if its an echo of ourselves from the master UBA, ignore. This should never happen mind you
    if (EMS_RxTelegram->src == EMS_ID_ME) {
        if (EMS_Sys_Status.emsLogging == EMS_SYS_LOGGING_JABBER)
            _debugPrintTelegram("echo: ", EMS_RxTelegram, COLOR_WHITE);
        return;
    }

    // if its a broadcast and we didn't just send anything, process it and exit
    if (EMS_Sys_Status.emsTxStatus == EMS_TX_STATUS_IDLE) {
        _ems_processTelegram(EMS_RxTelegram);
        return;
    }

    // release the lock on the TxQueue
    EMS_Sys_Status.emsTxStatus = EMS_TX_STATUS_IDLE;

    // at this point we can assume TxStatus was EMS_TX_STATUS_WAIT as we just sent a read or validate telegram
    // for READ or VALIDATE the dest (telegram[1]) is always us, so check for this
    // and if not we probably didn't get any response so remove the last Tx from the queue and process the telegram anyway
    if ((telegram[1] & 0x7F) != EMS_ID_ME) {
        _removeTxQueue();
        _ems_processTelegram(EMS_RxTelegram);
        return;
    }

    // first double check we actually have something in the Tx queue that we're waiting upon
    if (EMS_TxQueue.isEmpty()) {
        _ems_processTelegram(EMS_RxTelegram);
        return;
    }

    // get the Tx telegram we just sent
    _EMS_TxTelegram EMS_TxTelegram = EMS_TxQueue.first();

    // check action
    // if READ, match the current inbound telegram to what we just sent
    // if WRITE, should not happen
    // if VALIDATE, check the contents
    if (EMS_TxTelegram.action == EMS_TX_TELEGRAM_READ) {
        // remove MSB from src/dest
        if (((EMS_RxTelegram->src & 0x7F) == (EMS_TxTelegram.dest & 0x7F)) && (EMS_RxTelegram->type == EMS_TxTelegram.type)) {
            // all checks out, read was successful, remove tx from queue and continue to process telegram
            _removeTxQueue();
            EMS_Sys_Status.emsRxPgks++;         // increment Rx happy counter
            EMS_Sys_Status.emsTxCapable = true; // we're able to transmit a telegram on the Tx
        } else {
            // read not OK, we didn't get back a telegram we expected.
            // first see if we got a response back from the sender saying its an unknown command
            if (EMS_RxTelegram->data_length == 0) {
                _removeTxQueue();
            } else {
                // leave on queue and try again, but continue to process what we received as it may be important
                EMS_Sys_Status.txRetryCount++;
                // if retried too many times, give up and remove it
                if (EMS_Sys_Status.txRetryCount >= TX_WRITE_TIMEOUT_COUNT) {
                    if (EMS_Sys_Status.emsLogging >= EMS_SYS_LOGGING_BASIC) {
                        myDebug_P(PSTR("-> Read failed. Giving up and removing write from queue"));
                    }
                    _removeTxQueue();
                } else {
                    if (EMS_Sys_Status.emsLogging >= EMS_SYS_LOGGING_BASIC) {
                        myDebug_P(PSTR("-> Read failed. Retrying (%d/%d)..."), EMS_Sys_Status.txRetryCount, TX_WRITE_TIMEOUT_COUNT);
                    }
                }
            }
        }
        _ems_processTelegram(EMS_RxTelegram); // process it always
    }

    if (EMS_TxTelegram.action == EMS_TX_TELEGRAM_WRITE) {
        // should not get here, since this is handled earlier receiving a 01 or 04
        myDebug_P(PSTR("-> Write error - panic! should never get here"));
    }

    if (EMS_TxTelegram.action == EMS_TX_TELEGRAM_VALIDATE) {
        // this is a read telegram which we use to validate the last write

        // data block starts at position 5 for EMS1.0 and 6 for EMS2.0.
        // See https://github.com/proddy/EMS-ESP/wiki/RC3xx-Thermostats
        uint8_t dataReceived = (EMS_RxTelegram->emsplus) ? telegram[6] : telegram[4];

        if (EMS_TxTelegram.comparisonValue == dataReceived) {
            // validate was successful, the write changed the value
            _removeTxQueue(); // now we can remove the Tx validate command the queue
            if (EMS_Sys_Status.emsLogging >= EMS_SYS_LOGGING_BASIC) {
                myDebug_P(PSTR("-> Validate confirmed, last Write to 0x%02X was successful"), EMS_TxTelegram.dest);
            }
            // follow up with the post read command
            ems_doReadCommand(EMS_TxTelegram.comparisonPostRead, EMS_TxTelegram.dest);
        } else {
            // write failed
            if (EMS_Sys_Status.emsLogging >= EMS_SYS_LOGGING_BASIC) {
                myDebug_P(PSTR("-> Write failed. Compared set value 0x%02X with received value of 0x%02X"), EMS_TxTelegram.comparisonValue, dataReceived);
            }
            if (++EMS_Sys_Status.txRetryCount > TX_WRITE_TIMEOUT_COUNT) {
                if (EMS_Sys_Status.emsLogging >= EMS_SYS_LOGGING_BASIC) {
                    myDebug_P(PSTR("-> Write failed. Giving up, removing from queue"));
                }
                _removeTxQueue();
            } else {
                // retry, turn the validate back into a write and try again
                if (EMS_Sys_Status.emsLogging >= EMS_SYS_LOGGING_BASIC) {
                    myDebug_P(PSTR("-> Write didn't work, retrying (%d/%d)..."), EMS_Sys_Status.txRetryCount, TX_WRITE_TIMEOUT_COUNT);
                }
                EMS_TxTelegram.action    = EMS_TX_TELEGRAM_WRITE;
                EMS_TxTelegram.dataValue = EMS_TxTelegram.comparisonValue;  // restore old value
                EMS_TxTelegram.offset    = EMS_TxTelegram.comparisonOffset; // restore old value
                EMS_TxTelegram.type      = EMS_TxTelegram.type_validate;    // restore old value, we swapped them to save the original type

                EMS_TxQueue.shift();                 // remove validate from queue
                EMS_TxQueue.unshift(EMS_TxTelegram); // add back to queue making it next in line
            }
        }
    }

    ems_tx_pollAck(); // send Acknowledgement back to free the EMS bus since we have the telegram
}

/**
 * Send a command to UART Tx to Read from another device
 * Read commands when sent must respond by the destination (target) immediately (or within 10ms)
 */
void ems_doReadCommand(uint16_t type, uint8_t dest) {
    // if not a valid type of boiler is not accessible then quits
    if ((type == EMS_ID_NONE) || (dest == EMS_ID_NONE)) {
        return;
    }

    // if we're preventing all outbound traffic, quit
    if (EMS_Sys_Status.emsTxDisabled) {
        if (ems_getLogging() != EMS_SYS_LOGGING_NONE) {
            myDebug_P(PSTR("in Listen Mode. All Tx is disabled."));
        }
        return;
    }

    _EMS_TxTelegram EMS_TxTelegram = EMS_TX_TELEGRAM_NEW; // create new Tx
    EMS_TxTelegram.timestamp       = millis();            // set timestamp
    EMS_Sys_Status.txRetryCount    = 0;                   // reset retry counter

    // see if its a known type
    int8_t i = _ems_findType(type);

    if ((ems_getLogging() == EMS_SYS_LOGGING_BASIC) || (ems_getLogging() == EMS_SYS_LOGGING_VERBOSE)) {
        if (i == -1) {
            myDebug_P(PSTR("Requesting type (0x%02X) from dest 0x%02X"), type, dest);
        } else {
            myDebug_P(PSTR("Requesting type %s(0x%02X) from dest 0x%02X"), EMS_Types[i].typeString, type, dest);
        }
    }
    EMS_TxTelegram.action             = EMS_TX_TELEGRAM_READ; // read command
    EMS_TxTelegram.dest               = dest;                 // 8th bit will be set to indicate a read
    EMS_TxTelegram.offset             = 0;                    // 0 for all data
    EMS_TxTelegram.type               = type;
    EMS_TxTelegram.length             = EMS_MIN_TELEGRAM_LENGTH; // EMS 1.0: 6 bytes long (including CRC at end), EMS+ will add 2 bytes. includes CRC
    EMS_TxTelegram.dataValue          = EMS_MAX_TELEGRAM_LENGTH; // for a read this is the # bytes we want back
    EMS_TxTelegram.type_validate      = EMS_ID_NONE;
    EMS_TxTelegram.comparisonValue    = 0;
    EMS_TxTelegram.comparisonOffset   = 0;
    EMS_TxTelegram.comparisonPostRead = EMS_ID_NONE;

    EMS_TxQueue.push(EMS_TxTelegram);
}
