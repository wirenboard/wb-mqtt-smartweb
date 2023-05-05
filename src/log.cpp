#include "log.h"

WBMQTT::TLogger ErrorMqttToSw("ERROR: [MQTT->SW] ", WBMQTT::TLogger::StdErr, WBMQTT::TLogger::RED);
WBMQTT::TLogger WarnMqttToSw("WARNING: [MQTT->SW] ", WBMQTT::TLogger::StdErr, WBMQTT::TLogger::YELLOW);
WBMQTT::TLogger InfoMqttToSw("INFO: [MQTT->SW] ", WBMQTT::TLogger::StdErr, WBMQTT::TLogger::GREY);
WBMQTT::TLogger DebugMqttToSw("DEBUG: [MQTT->SW] ", WBMQTT::TLogger::StdErr, WBMQTT::TLogger::WHITE, false);

WBMQTT::TLogger ErrorSwToMqtt("ERROR: [SW->MQTT] ", WBMQTT::TLogger::StdErr, WBMQTT::TLogger::RED);
WBMQTT::TLogger WarnSwToMqtt("WARNING: [SW->MQTT] ", WBMQTT::TLogger::StdErr, WBMQTT::TLogger::YELLOW);
WBMQTT::TLogger InfoSwToMqtt("INFO: [SW->MQTT] ", WBMQTT::TLogger::StdErr, WBMQTT::TLogger::GREY);
WBMQTT::TLogger DebugSwToMqtt("DEBUG: [SW->MQTT] ", WBMQTT::TLogger::StdErr, WBMQTT::TLogger::WHITE, false);