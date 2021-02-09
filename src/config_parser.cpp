#include "config_parser.h"

#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "log.h"

#define LOG(logger) ::logger.Log() << "[config] "

namespace
{
    bool EndsWith(const std::string& str, const std::string& suffix)
    {
        return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::unique_ptr<ISmartWebCodec> GetCodec(const Json::Value& data)
    {
        if (data.isMember("encoding")) {
            auto enc = data["encoding"].asString();

            if (WBMQTT::StringStartsWith(enc, "schedule")) {
                throw std::runtime_error("Encoding '" + enc + "' is not supported");
            }
            if (enc == "byte")     return std::make_unique<TIntCodec<int8_t,   1, 0>>();
            if (enc == "short")    return std::make_unique<TIntCodec<int16_t,  1, 0>>();
            if (enc == "short10")  return std::make_unique<TIntCodec<int16_t,  10, 1>>();
            if (enc == "short100") return std::make_unique<TIntCodec<int16_t,  100, 2>>();
            if (enc == "ushort")   return std::make_unique<TIntCodec<uint16_t, 1, 0>>();
            if (enc == "uint1K")   return std::make_unique<TIntCodec<uint32_t, 1000, 3>>();
            if (enc == "uint60K")  return std::make_unique<TIntCodec<uint32_t, 60000, 6>>();
            if (enc == "ubyte") {
                if (data.isMember("values")) {
                    std::map<uint8_t, std::string> values;
                    const auto& ar = data["values"];
                    for (Json::Value::const_iterator it = ar.begin(); it != ar.end(); ++it) {
                        values.insert({atoi(it.name().c_str()), it->asString()});
                    }
                    return std::make_unique<TEnumCodec>(values);
                }
                return std::make_unique<TIntCodec<uint8_t, 1, 0>>();
            }
        }
        return std::make_unique<TIntCodec<int16_t, 10, 1>>(); // default codec
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
            LOG(Debug) << "Input '" << p->Name << "' " << p->Type << " id " << p->Id;
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
            LOG(Debug) << "Output '" << p->Name << "' " << p->Type << " id " << p->Id;
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
                LOG(Debug) << "Parameter '" << p->Name << "', " << p->Type 
                            << ", id " << p->Id 
                            << ", " << p->Codec->GetName()
                            << (p->ReadOnly ? ", read only" : "");
                programClass->Parameters.insert({p->Id, p});
                maxId = std::max(maxId, p->Id);
            } catch (const std::exception& e) {
                LOG(Warn) << "Parameter '" << it.name() << "' is ignored. " << e.what();
            }
        }
        return orderBase + maxId + 1;
    }

    void LoadSmartWebToMqttConfig(TSmartWebToMqttConfig& config,
                                  const Json::Value&     configJson,
                                  const std::string&     classesDir,
                                  const Json::Value&     classSchema)
    {
        if(configJson.isMember("poll_interval_ms")) {
            config.PollInterval = std::chrono::milliseconds(configJson["poll_interval_ms"].asUInt());
        }

        DIR *dir;
        struct dirent *dirp;
        struct stat filestat;

        if ((dir = opendir(classesDir.c_str())) == NULL)
        {
            LOG(Warn) << ("Cannot open " + classesDir + " directory");
            return;
        }

        while ((dirp = readdir(dir))) {
            std::string dname = dirp->d_name;
            if(!EndsWith(dname, ".json"))
                continue;

            std::string filepath = classesDir + "/" + dname;
            if (stat(filepath.c_str(), &filestat)) {
                continue;
            }
            if (S_ISDIR(filestat.st_mode)) {
                continue;
            }

            try {
                auto classJson = WBMQTT::JSON::Parse(filepath);
                WBMQTT::JSON::Validate(classJson, classSchema);
                LoadSmartWebClass(config, classJson);
            } catch (const std::exception& e) {
                LOG(Error) << "Failed to parse " << filepath << "\n" << e.what();
                continue;
            }
        }
        closedir(dir);
    }

    void LoadMqttToSmartWebConfig(TMqttToSmartWebConfig& config, const Json::Value& configJson)
    {
        const auto& mappings = configJson["mappings"];

        for (Json::ArrayIndex i = 0; i < mappings.size(); ++i) {
            auto mapping = mappings[i];

            const auto& mqtt = mapping["mqtt"];
            const auto& mqtt_channel = mqtt["channel"].asString();

            auto& mqtt_channel_timing = config.MqttChannelsTiming[mqtt_channel];
            mqtt_channel_timing.refresh_last_update_timepoint();
            if (mqtt.isMember("value_timeout_min")) {
                mqtt_channel_timing.ValueTimeoutMin = TTimeIntervalMin(mqtt["value_timeout_min"].asInt());
            }

            if (mapping.isMember("parameter")) {
                SmartWeb::TParameterInfo parameter_info {0};
                const auto& parameter = mapping["parameter"];
                if (!parameter.isMember("parameter_id")) {
                    throw std::runtime_error("parameter_id is misssing");
                }
                if (!parameter.isMember("program_type")) {
                    throw std::runtime_error("program_type is misssing");
                }
                if (!parameter.isMember("parameter_index")) {
                    throw std::runtime_error("parameter_index is misssing");
                }
                parameter_info.parameter_id = parameter["parameter_id"].asUInt();
                parameter_info.program_type = parameter["program_type"].asUInt();
                parameter_info.index = parameter["parameter_index"].asUInt();

                LOG(Info) << "Map parameter {"
                        << "program_type: " << (int)parameter_info.program_type << ", "
                        << "parameter_id: " << (int)parameter_info.parameter_id << ", "
                        << "parameter_index: " << (int)parameter_info.index << ", "
                        << "raw " << (int)parameter_info.raw
                        << "} to {channel: " << mqtt_channel << "};";

                if (config.ParameterMapping.count(parameter_info.raw)) {
                    throw std::runtime_error("Malformed JSON config: duplicate parameter");
                }

                config.ParameterMapping[parameter_info.raw].from_string(mqtt_channel);
                config.ParameterCount = std::max(config.ParameterCount, uint8_t(parameter_info.index + 1));
            }

            if (mapping.isMember("sensor")) {
                SmartWeb::TParameterInfo parameter_info {0};
                parameter_info.parameter_id = SmartWeb::Controller::Parameters::SENSOR;
                parameter_info.program_type = SmartWeb::PT_CONTROLLER;
                parameter_info.index = mapping["sensor"]["index"].asUInt();

                LOG(Info) << "Map sensor {"
                        << "parameter_index: " << (int)parameter_info.index << ", "
                        << "raw " << (int)parameter_info.raw
                        << "} to {channel: " << mqtt_channel << "};";

                if (config.ParameterMapping.count(parameter_info.raw)) {
                    throw std::runtime_error("Malformed JSON config: duplicate sensor");
                }

                config.ParameterMapping[parameter_info.raw].from_string(mqtt_channel);
                config.ParameterCount = std::max(config.ParameterCount, uint8_t(parameter_info.index + 1));
            }

            if (mapping.isMember("output")) {
                auto channel_id = mapping["output"]["channel_id"].asUInt();

                LOG(Info) << "Map output {"
                        << "channel_id: " << (int)channel_id
                        << "} to {channel: " << mqtt_channel << "};";

                if (config.OutputMapping[channel_id].is_initialized()) {
                    throw std::runtime_error("Malformed JSON config: duplicate output" );
                }

                config.OutputMapping[channel_id].from_string(mqtt_channel);
            }

            if (!mapping.isMember("parameter") && !mapping.isMember("sensor") && !mapping.isMember("output")) {
                throw std::runtime_error("Malformed JSON config: no parameter, sensor or output in mapping");
            }
        }
    }
}

void LoadSmartWebClass(TSmartWebToMqttConfig& config, const Json::Value& data)
{
    auto programType = data["programType"].asUInt();
    if (config.Classes.count(programType)) {
        LOG(Warn) << "Program type: " << programType << "is already defined";
        return;
    }

    auto cl = std::make_shared<TSmartWebClass>();
    cl->Type = programType;
    cl->Name = data["class"].asString();
    LOG(Debug) << "Loading class '" << cl->Name << "' (program type = " << programType << ")";

    if (data.isMember("implements")) {
        for (const auto& parent: data["implements"]) {
            cl->ParentClasses.push_back(parent.asString());
        }
    }

    uint32_t orderBase = LoadInputs(data, cl.get());
    orderBase = LoadOutputs(data, cl.get(), orderBase);
    LoadParameters(data, cl.get(), orderBase);
    
    config.Classes.insert({programType, cl});
    LOG(Info) << "Class '" << cl->Name << "' (program type = " << programType << ") is loaded";
}

void LoadConfig(TConfig&           config,
                const std::string& configFileName,
                const std::string& configSchemaFileName,
                const std::string& classSchemaFileName)
{
    Json::Value configJson = WBMQTT::JSON::Parse(configFileName);
    WBMQTT::JSON::Validate(configJson, WBMQTT::JSON::Parse(configSchemaFileName));

    config.MqttToSmartWeb.ProgramId = configJson["controller_id"].asUInt();

    LoadMqttToSmartWebConfig(config.MqttToSmartWeb, configJson);

    Json::Value classSchema = WBMQTT::JSON::Parse(classSchemaFileName);
    LoadSmartWebToMqttConfig(config.SmartWebToMqtt, configJson, configFileName + ".d/classes", classSchema);
}