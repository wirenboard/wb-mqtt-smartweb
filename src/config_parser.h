#pragma once

#include "MqttToSmartWebGateway.h"
#include "SmartWebToMqttGateway.h"
#include <wblib/wbmqtt.h>
#include <wblib/json_utils.h>

struct TConfig
{
    TMqttToSmartWebConfig        MqttToSmartWeb;
    TSmartWebToMqttConfig        SmartWebToMqtt;
    WBMQTT::TMosquittoMqttConfig Mqtt;
    bool                         Debug = false;
};

void LoadSmartWebClass(TSmartWebToMqttConfig& config, const Json::Value& data);

void LoadConfig(TConfig&           consfig,
                const std::string& configFileName,
                const std::string& configSchemaFileName,
                const std::string& classSchemaFileName);
