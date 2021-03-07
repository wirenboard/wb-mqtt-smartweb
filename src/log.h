#pragma once

#include <wblib/log.h>

extern WBMQTT::TLogger ErrorMqttToSw;
extern WBMQTT::TLogger WarnMqttToSw;
extern WBMQTT::TLogger InfoMqttToSw;
extern WBMQTT::TLogger DebugMqttToSw;

extern WBMQTT::TLogger ErrorSwToMqtt;
extern WBMQTT::TLogger WarnSwToMqtt;
extern WBMQTT::TLogger InfoSwToMqtt;
extern WBMQTT::TLogger DebugSwToMqtt;