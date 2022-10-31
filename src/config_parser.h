#pragma once

#include "MqttToSmartWebGateway.h"
#include "SmartWebToMqttGateway.h"
#include <wblib/wbmqtt.h>
#include <wblib/json_utils.h>

struct TConfig
{
    std::vector<TMqttToSmartWebConfig> Controllers;
    TSmartWebToMqttConfig              SmartWebToMqtt;
    WBMQTT::TMosquittoMqttConfig       Mqtt;
    bool                               Debug = false;
};

void LoadSmartWebClass(TSmartWebToMqttConfig& config, const Json::Value& data);

void LoadConfig(TConfig& config,
                const std::string& configFilePath,
                const std::string& pathToDeviceClassDirectory,
                const std::string& configSchemaFileName,
                const std::string& classSchemaFileName);
