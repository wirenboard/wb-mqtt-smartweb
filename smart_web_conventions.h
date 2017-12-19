#pragma once
#include <stdint.h>

namespace SmartWeb {
    enum E_MessageFormat : uint8_t
    {
        MF_FORMAT_0,
        MF_LONG,
    };

    enum E_ProgramType : uint8_t
    {
        PT_CAN_PROGRAM_TYPE_UNDEFINED = 0,
        PT_PROGRAM = 1,
        PT_OUTDOOR_SENSOR = 2,
        PT_CONSUMER = 3,
        PT_CASCADE_MANAGER = 4,
        PT_ROOM_DEVICE = 5,
        PT_TEMPERATURE_SOURCE = 6,
        PT_HEAT_ACCUMULATOR = 7,
        PT_EXTENDED_CONTROLLER = 8,
        PT_EXTENSION_CONTROLLER = 9,
        PT_MONITORING_DEVICE = 10,
        PT_CONTROLLER = 11,
        PT_CIRCUIT = 12,
        PT_SCHEDULE = 13,
        PT_HEATING_CIRCUIT = 14,
        PT_DIRECT_CIRCUIT = 15,
        PT_DHW = 16,
        PT_FLOW_THROUGH_DHW = 17,
        PT_TEMPERATURE_GENERATOR = 18,
        PT_POOL = 19,
        PT_THERMOSTAT = 20,
        PT_SNOWMELT = 21,
        PT_REMOTE_CONTROL = 22,
        PT_BOILER = 23,
        PT_CHILLER = 24,
        PT_SOLAR_COLLECTOR = 25,
        PT_VENTILATION = 26,
        PT_GENERIC_RELAY = 27,
        PT_ALARM = 28,
    };

    union TCanHeader
    {
        uint32_t raw;
        struct
        {
            uint8_t program_type : 8,
                    program_id : 8,
                    function_id : 8;
            union {
                struct
                {
                    uint8_t reserve : 3,
                            exception_flag : 1,
                            response_flag : 1,
                            tra : 3;
                };

                struct
                {
                    uint8_t message_format : 3,
                            message_type : 2,
                            trash : 3;
                };
            };
        } rec;
    };
}
