#include "config_parser.h"

#include <experimental/filesystem>
#include <regex>

#include "log.h"

#define LOG(logger) ::logger.Log() << "[config] "

namespace
{
    std::unique_ptr<ISmartWebCodec> GetCodec(const Json::Value& data)
    {
        if (data.isMember("encoding")) {
            auto enc = data["encoding"].asString();

            if (WBMQTT::StringStartsWith(enc, "schedule")) {
                throw std::runtime_error("Encoding '" + enc + "' is not supported");
            }
            if (enc == "byte")     return std::make_unique<TIntCodec<int8_t,   1>>();
            if (enc == "short")    return std::make_unique<TIntCodec<int16_t,  1>>();
            if (enc == "short10")  return std::make_unique<TIntCodec<int16_t,  10>>();
            if (enc == "short100") return std::make_unique<TIntCodec<int16_t,  100>>();
            if (enc == "ushort")   return std::make_unique<TIntCodec<uint16_t, 1>>();
            if (enc == "uint1K")   return std::make_unique<TIntCodec<uint32_t, 1000>>();
            if (enc == "uint60K")  return std::make_unique<TIntCodec<uint32_t, 60000>>();
            if (enc == "ubyte") {
                if (data.isMember("values")) {
                    std::map<uint8_t, std::string> values;
                    const auto& ar = data["values"];
                    for (Json::Value::const_iterator it = ar.begin(); it != ar.end(); ++it) {
                        values.insert({atoi(it.name().c_str()), it->asString()});
                    }
                    return std::make_unique<TEnumCodec>(values);
                }
                return std::make_unique<TIntCodec<uint8_t, 1>>();
            }
        }
        return std::make_unique<TIntCodec<int16_t, 10>>(); // default codec
    }

    std::shared_ptr<TSmartWebParameter> LoadParameter(const Json::Value&    param,
                                                      const std::string&    name,
                                                      const TSmartWebClass* programClass,
                                                      uint32_t              orderBase)
    {
        auto p = std::make_shared<TSmartWebParameter>();
        p->Id    = param["id"].asUInt();
        p->Name  = name;
        p->Type  = param.get("type", "value").asString();
        p->ProgramClass = programClass;
        p->Order = orderBase + p->Id;
        return p;
    }

    uint32_t LoadInputs(const Json::Value& data, TSmartWebClass* programClass)
    {
        if (!data.isMember("inputs")) {
            return 0;
        }
        uint32_t maxId = 0;
        const auto& ar = data["inputs"];
        for (Json::Value::const_iterator it = ar.begin(); it != ar.end(); ++it) {
            auto p = LoadParameter(*it, it.name(), programClass, 0);
            if (p->Type == "onOff") {
                p->Codec = std::make_unique<TOnOffSensorCodec>();
            } else {
                p->Codec = std::make_unique<TSensorCodec>();
            }
            LOG(WBMQTT::Debug) << "Input '" << p->Name << "' " << p->Type << " id " << p->Id;
            programClass->Inputs.insert({p->Id, p});
            maxId = std::max(maxId, p->Id);
        }
        return maxId + 1;
    }

    uint32_t LoadOutputs(const Json::Value& data, TSmartWebClass* programClass, uint32_t orderBase)
    {
        if (!data.isMember("outputs")) {
            return orderBase;
        }
        uint32_t maxId = 0;
        const auto& ar = data["outputs"];
        for (Json::Value::const_iterator it = ar.begin(); it != ar.end(); ++it) {
            auto p = LoadParameter(*it, it.name(), programClass, orderBase);
            if (p->Type == "PWM") {
                p->Codec = std::make_unique<TPwmCodec>();
            } else {
                p->Codec = std::make_unique<TOutputCodec>();
            }
            LOG(WBMQTT::Debug) << "Output '" << p->Name << "' " << p->Type << " id " << p->Id;
            programClass->Outputs.insert({p->Id, p});
            maxId = std::max(maxId, p->Id);
        }
        return orderBase + maxId + 1;
    }

    uint32_t LoadParameters(const Json::Value& data, TSmartWebClass* programClass, uint32_t orderBase)
    {
        if (!data.isMember("parameters")) {
            return orderBase;
        }
        uint32_t maxId = 0;
        const auto& ar = data["parameters"];
        for (Json::Value::const_iterator it = ar.begin(); it != ar.end(); ++it) {
            try {
                auto p = LoadParameter(*it, it.name(), programClass, orderBase);
                p->ReadOnly = false;
                WBMQTT::JSON::Get((*it), "readOnly", p->ReadOnly);
                    p->Codec = GetCodec(*it);
                if (p->Type == "onOff") {
                    p->Codec = std::make_unique<TOnOffSensorCodec>();
                }
                if (p->Type == "temperature" && p->ReadOnly) {
                    p->Codec = std::make_unique<TSensorCodec>();
                }
                LOG(WBMQTT::Debug) << "Parameter '" << p->Name << "', " << p->Type 
                            << ", id " << p->Id 
                            << ", " << p->Codec->GetName()
                            << (p->ReadOnly ? ", read only" : "");
                programClass->Parameters.insert({p->Id, p});
                maxId = std::max(maxId, p->Id);
            } catch (const std::exception& e) {
                LOG(WBMQTT::Warn) << "Parameter '" << it.name() << "' is ignored. " << e.what();
            }
        }
        return orderBase + maxId + 1;
    }

    /// Сalls a function for each file in a directory
    /// \param dirPath directory to scan
    /// \param fileExtension file extension for which the function is called.<br>
    ///                      Includes a dot at the beginning of a line.<br>
    ///                      For example: ".json"
    /// \param scanFunc function to be called for files
    void ScanFileInDirectory(
        const std::experimental::filesystem::path& dirPath,
        const std::string& fileExtension,
        const std::function<void(const std::experimental::filesystem::path& filePath)>& scanFunc)
    {
        for (const auto& entry: std::experimental::filesystem::directory_iterator(dirPath)) {
            const auto fileName = entry.path().string();
            if (std::experimental::filesystem::is_regular_file(entry) && (entry.path().extension() == fileExtension)) {
                scanFunc(entry.path());
            }
        }
    }

    void LoadSmartWebToMqttConfig(TSmartWebToMqttConfig& config,
                                  const Json::Value& configJson,
                                  const std::string& classesDir,
                                  const Json::Value& classSchema,
                                  TDeviceClassOwner owner)
    {
        if (configJson.isMember("poll_interval_ms")) {
            config.PollInterval = std::chrono::milliseconds(configJson["poll_interval_ms"].asUInt());
        }

        try {
            const std::experimental::filesystem::path dirPath{classesDir};

            ScanFileInDirectory(dirPath,
                                ".json",
                                [classSchema, &config, owner](const std::experimental::filesystem::path& filePath) {
                                    try {
                                        auto classJson = WBMQTT::JSON::Parse(filePath);
                                        WBMQTT::JSON::Validate(classJson, classSchema);
                                        LoadSmartWebClass(config, classJson, owner);
                                    } catch (const std::exception& e) {
                                        LOG(WBMQTT::Error) << "Failed to parse " << filePath << "\n" << e.what();
                                    }
                                });
        } catch (std::experimental::filesystem::filesystem_error const& ex) {
            LOG(WBMQTT::Error) << "Cannot open " << classesDir << " directory: " << ex.what();
            return;
        }
    }

    void LoadTiming(TMqttToSmartWebConfig& controller, const std::string& mqtt_channel, const Json::Value& configJson)
    {
        auto& mqtt_channel_timing = controller.MqttChannelsTiming[mqtt_channel];
        mqtt_channel_timing.refresh_last_update_timepoint();
        if (configJson.isMember("value_timeout_min")) {
            mqtt_channel_timing.ValueTimeoutMin = TTimeIntervalMin(configJson["value_timeout_min"].asInt());
        }
    }

    TMqttToSmartWebConfig LoadMqttToSmartWebController(const Json::Value& configJson)
    {
        TMqttToSmartWebConfig res;
        res.ProgramId = configJson["controller_id"].asUInt();

        if (configJson.isMember("sensors")) {
            for (const auto& sensor: configJson["sensors"]) {
                const auto& mqtt_channel = sensor["channel"].asString();
                LoadTiming(res, mqtt_channel, sensor);
                SmartWeb::TParameterInfo parameter_info {0};
                parameter_info.parameter_id = SmartWeb::Controller::Parameters::SENSOR;
                parameter_info.program_type = SmartWeb::PT_CONTROLLER;
                parameter_info.index = sensor["sensor_index"].asUInt();

                LOG(WBMQTT::Info) << "Controller: " << (int)res.ProgramId << " map sensor {"
                        << "parameter_index: " << (int)parameter_info.index << ", "
                        << "raw " << (int)parameter_info.raw
                        << "} to {channel: " << mqtt_channel << "};";

                if (res.ParameterMapping.count(parameter_info.raw)) {
                    throw std::runtime_error("Malformed JSON config: duplicate sensor");
                }

                res.ParameterMapping[parameter_info.raw].from_string(mqtt_channel);
                res.ParameterCount = std::max(res.ParameterCount, uint8_t(parameter_info.index + 1));

                // Sensor is also accesible as output with index = sensor_index - 1
                auto outputIndex = parameter_info.index - 1;

                if (res.OutputMapping[outputIndex].is_initialized()) {
                    throw std::runtime_error("Malformed JSON config: duplicate output " + std::to_string(outputIndex));
                }

                res.OutputMapping[outputIndex].from_string(mqtt_channel);
            }
        }

        if (configJson.isMember("parameters")) {
            for (const auto& parameter: configJson["parameters"]) {
                const auto& mqtt_channel = parameter["channel"].asString();
                LoadTiming(res, mqtt_channel, configJson);
                SmartWeb::TParameterInfo parameter_info {0};
                parameter_info.parameter_id = parameter["parameter_id"].asUInt();
                parameter_info.program_type = parameter["program_type"].asUInt();
                parameter_info.index = parameter["parameter_index"].asUInt();

                LOG(WBMQTT::Info) << "Controller: " << (int)res.ProgramId << " map parameter {"
                        << "program_type: " << (int)parameter_info.program_type << ", "
                        << "parameter_id: " << (int)parameter_info.parameter_id << ", "
                        << "parameter_index: " << (int)parameter_info.index << ", "
                        << "raw " << (int)parameter_info.raw
                        << "} to {channel: " << mqtt_channel << "};";

                if (res.ParameterMapping.count(parameter_info.raw)) {
                    throw std::runtime_error("Malformed JSON config: duplicate parameter");
                }

                res.ParameterMapping[parameter_info.raw].from_string(mqtt_channel);
                res.ParameterCount = std::max(res.ParameterCount, uint8_t(parameter_info.index + 1));
            }
        }

        if (!configJson.isMember("parameters") && !configJson.isMember("sensors") && !configJson.isMember("outputs")) {
            throw std::runtime_error("Malformed JSON config: no parameter, sensor or output in mapping");
        }
        return res;
    }

    void LoadMqttToSmartWebConfig(TConfig& config, const Json::Value& configJson)
    {
        if(configJson.isMember("debug")) {
            config.Debug = configJson["debug"].asBool();
        }

        for (const auto& controller: configJson["controllers"]) {
            try {
                config.Controllers.push_back(LoadMqttToSmartWebController(controller));
            } catch (std::exception& e) {
                LOG(WBMQTT::Error) << e.what();
            }
        }
    }
}

void LoadSmartWebClass(TSmartWebToMqttConfig& config, const Json::Value& data, TDeviceClassOwner owner)
{
    const auto programType = data["programType"].asUInt();
    const auto programName = data["class"].asString();

    auto classIt = config.Classes.find(programType);

    if (classIt != config.Classes.end()) {
        if (classIt->second->Owner == owner) {
            LOG(WBMQTT::Warn) << "Program type: " << programType << " is already defined";
            return;
        } else {
            LOG(WBMQTT::Info) << "Overriding a built-in device class '" << programName << "' in *.d/classes";
            if (owner != TDeviceClassOwner::USER) {
                return;
            }
        }
    }

    auto cl = std::make_shared<TSmartWebClass>();
    cl->Type = programType;
    cl->Name = programName;
    cl->Owner = owner;
    LOG(WBMQTT::Debug) << "Loading class '" << cl->Name << "' (program type = " << programType << ")";

    if (data.isMember("implements")) {
        for (const auto& parent: data["implements"]) {
            cl->ParentClasses.push_back(parent.asString());
        }
    }

    uint32_t orderBase = LoadInputs(data, cl.get());
    orderBase = LoadOutputs(data, cl.get(), orderBase);
    LoadParameters(data, cl.get(), orderBase);

    if (classIt != config.Classes.end()) {
        classIt->second = cl;
    } else {
        config.Classes.insert({programType, cl});
    }

    LOG(WBMQTT::Info) << "Class '" << cl->Name << "' (program type = " << programType << ") is loaded";
}

void LoadConfig(TConfig& config,
                const std::string& configFilePath,
                const std::string& pathToDeviceClassDirectory,
                const std::string& pathToBuiltInDeviceClassDirectory,
                const std::string& configSchemaFileName,
                const std::string& classSchemaFileName)
{
    Json::Value configJson = WBMQTT::JSON::Parse(configFilePath);
    WBMQTT::JSON::Validate(configJson, WBMQTT::JSON::Parse(configSchemaFileName));

    LoadMqttToSmartWebConfig(config, configJson);

    Json::Value classSchema = WBMQTT::JSON::Parse(classSchemaFileName);
    LoadSmartWebToMqttConfig(config.SmartWebToMqtt,
                             configJson,
                             pathToDeviceClassDirectory,
                             classSchema,
                             TDeviceClassOwner::USER);
    LoadSmartWebToMqttConfig(config.SmartWebToMqtt,
                             configJson,
                             pathToBuiltInDeviceClassDirectory,
                             classSchema,
                             TDeviceClassOwner::BUILTIN);
}