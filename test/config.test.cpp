#include "config_parser.h"

#include <gtest/gtest.h>
#include <vector>

#include <wblib/json_utils.h>
#include <wblib/testing/testlog.h>

using namespace WBMQTT;

namespace
{

    struct TestClassParameterSample
    {
        uint32_t id;
        std::string name;
        std::string type;
        uint32_t order;
        std::string programClassName;
        std::string codecName;
        bool readOnly;
    };
}

class TLoadConfigTest: public Testing::TLoggedFixture
{
protected:
    std::string TestRootDir;
    std::string SchemaFile;
    std::string ClassSchemaFile;

    void SetUp()
    {
        TestRootDir = Testing::TLoggedFixture::GetDataFilePath("config_test_data");
        SchemaFile = TestRootDir + "/../../wb-mqtt-smartweb.schema.json";
        ClassSchemaFile = TestRootDir + "/../../wb-mqtt-smartweb-class.schema.json";
    }

    std::shared_ptr<TSmartWebToMqttConfig> GetTestConfig()
    {
        auto classJson = WBMQTT::JSON::Parse(TestRootDir + "/classes/ROOM_DEVICE.json");
        auto pConfig = std::make_shared<TSmartWebToMqttConfig>();
        LoadSmartWebClass(*pConfig, classJson, TDeviceClassSource::USER);
        EXPECT_EQ(1, pConfig->Classes.size());
        return pConfig;
    }

    void ParameterEqHelper(const TestClassParameterSample& sample, const TSmartWebParameter& parameter)
    {
        EXPECT_EQ(sample.id, parameter.Id);
        EXPECT_EQ(sample.name, parameter.Name);
        EXPECT_EQ(sample.type, parameter.Type);
        EXPECT_EQ(sample.order, parameter.Order);
        EXPECT_EQ(sample.programClassName, parameter.ProgramClass->Name);
        EXPECT_EQ(sample.codecName, parameter.Codec->GetName());
        EXPECT_EQ(sample.readOnly, parameter.ReadOnly);
    }
};

TEST_F(TLoadConfigTest, ClassValidation)
{
    auto classSchema = WBMQTT::JSON::Parse(ClassSchemaFile);

    auto classJsonGood = WBMQTT::JSON::Parse(TestRootDir + "/classes/ROOM_DEVICE.json");
    WBMQTT::JSON::Validate(classJsonGood, classSchema);

    // missing fields
    for (size_t i = 1; i <= 8; ++i) {
        auto classJsonBad = WBMQTT::JSON::Parse(TestRootDir + "/classes/bad/bad" + std::to_string(i) + ".json");
        ASSERT_THROW(WBMQTT::JSON::Validate(classJsonBad, classSchema), std::runtime_error) << i;
    }
}

TEST_F(TLoadConfigTest, SmartWebToMqttConfigWoDat)
{
    auto classJson = WBMQTT::JSON::Parse(TestRootDir + "/classes/ROOM_DEVICE.json");
    TSmartWebToMqttConfig config;
    LoadSmartWebClass(config, classJson, TDeviceClassSource::USER);
    ASSERT_EQ(1, config.Classes.size());
    auto c = config.Classes.begin();
    const auto id = c->first;
    const auto smartWebClass = c->second;
    EXPECT_EQ(5, id);
    EXPECT_EQ("ROOM_DEVICE", smartWebClass->Name);
    EXPECT_EQ(5, smartWebClass->Type);
    ASSERT_EQ(1, smartWebClass->ParentClasses.size());
    auto parentClassIt = smartWebClass->ParentClasses.begin();
    EXPECT_EQ("PROGRAM", *parentClassIt);
}

TEST_F(TLoadConfigTest, SmartWebToMqttConfigInputs)
{
    auto config = GetTestConfig();
    const auto smartWebClass = config->Classes.begin()->second;

    EXPECT_EQ(7, smartWebClass->Inputs.size());
    auto input = smartWebClass->Inputs.at(2);

    TestClassParameterSample sample = {.id = 2,
                                       .name = "floorT",
                                       .type = "temperature",
                                       .order = 2,
                                       .programClassName = "ROOM_DEVICE",
                                       .codecName = "TSensorCodec",
                                       .readOnly = true};

    ParameterEqHelper(sample, *input);
}

TEST_F(TLoadConfigTest, SmartWebToMqttConfigOutputs)
{
    auto config = GetTestConfig();
    const auto smartWebClass = config->Classes.begin()->second;

    EXPECT_EQ(7, smartWebClass->Outputs.size());
    auto output = smartWebClass->Outputs.at(2);

    TestClassParameterSample sample = {.id = 2,
                                       .name = "addValve",
                                       .type = "relay",
                                       .order = 9,
                                       .programClassName = "ROOM_DEVICE",
                                       .codecName = "TOutputCodec",
                                       .readOnly = true};

    ParameterEqHelper(sample, *output);
}

TEST_F(TLoadConfigTest, SmartWebToMqttConfigParameters)
{
    auto config = GetTestConfig();
    const auto smartWebClass = config->Classes.begin()->second;

    EXPECT_EQ(34, smartWebClass->Parameters.size());
    auto parameter = smartWebClass->Parameters.at(2);

    TestClassParameterSample sample = {.id = 2,
                                       .name = "roomReducedTemperature",
                                       .type = "temperature",
                                       .order = 16,
                                       .programClassName = "ROOM_DEVICE",
                                       .codecName = "TIntCodec<signed 2, 10>",
                                       .readOnly = false};

    ParameterEqHelper(sample, *parameter);
}

TEST_F(TLoadConfigTest, LoadConfig)
{
    TConfig config;
    EXPECT_NO_THROW(LoadConfig(config,
                               TestRootDir + "/test_config.json",
                               TestRootDir + "/classes",
                               TestRootDir + "/builtin_classes",
                               SchemaFile,
                               ClassSchemaFile));

    EXPECT_TRUE(config.Debug);
    EXPECT_EQ(123, config.SmartWebToMqtt.PollInterval.count());

    EXPECT_EQ(3, config.SmartWebToMqtt.Classes.size());
    auto outdoorSensorDeviceClass = config.SmartWebToMqtt.Classes[2];

    ASSERT_NE(nullptr, outdoorSensorDeviceClass);
    EXPECT_EQ("OUTDOOR_SENSOR", outdoorSensorDeviceClass->Name);
    EXPECT_EQ(1, outdoorSensorDeviceClass->Inputs.size());
    EXPECT_EQ(0, outdoorSensorDeviceClass->Outputs.size());
    EXPECT_EQ(1, outdoorSensorDeviceClass->Parameters.size());

    auto roomDeviceClass = config.SmartWebToMqtt.Classes[5];
    ASSERT_NE(nullptr, roomDeviceClass);
    EXPECT_EQ("ROOM_DEVICE", roomDeviceClass->Name);
    EXPECT_EQ(7, roomDeviceClass->Inputs.size());
    EXPECT_EQ(7, roomDeviceClass->Outputs.size());
    EXPECT_EQ(34, roomDeviceClass->Parameters.size());

    auto temperatureSourceClass = config.SmartWebToMqtt.Classes[6];
    ASSERT_NE(nullptr, temperatureSourceClass);
    EXPECT_EQ("TEMPERATURE_SOURCE", temperatureSourceClass->Name);
    EXPECT_EQ(0, temperatureSourceClass->Inputs.size());
    EXPECT_EQ(0, temperatureSourceClass->Outputs.size());
    EXPECT_EQ(6, temperatureSourceClass->Parameters.size());
}