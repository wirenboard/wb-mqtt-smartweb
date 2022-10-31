#include "config_parser.h"

#include <gtest/gtest.h>
#include <vector>

#include <wblib/testing/testlog.h>
#include <wblib/testing/fake_driver.h>
#include <wblib/testing/fake_mqtt.h>
#include <wblib/json_utils.h>

using namespace WBMQTT;

namespace {

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

class TLoadConfigTest : public Testing::TLoggedFixture
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
        LoadSmartWebClass(*pConfig, classJson);
        EXPECT_LE(1, pConfig->Classes.size());
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

    // good class
    auto classJson = WBMQTT::JSON::Parse(TestRootDir + "/classes/ROOM_DEVICE.json");
    WBMQTT::JSON::Validate(classJson, classSchema);

    // missing fields
    for (size_t i = 1; i <= 8; ++i) {
        auto classJson = WBMQTT::JSON::Parse(TestRootDir + "/classes/bad/bad" + std::to_string(i) + ".json");
        ASSERT_THROW(WBMQTT::JSON::Validate(classJson, classSchema), std::runtime_error) << i;
    }
}

TEST_F(TLoadConfigTest, SmartWebToMqttConfig) {
    auto classJson = WBMQTT::JSON::Parse(TestRootDir + "/classes/ROOM_DEVICE.json");
    TSmartWebToMqttConfig config;
    LoadSmartWebClass(config, classJson);
    for (const auto& c: config.Classes) {
        Emit() << "Id: " << (int)c.first;
        Emit() << "Name: " << c.second->Name;
        Emit() << "Type: " << (int)c.second->Type;
        for (const auto& imp: c.second->ParentClasses) {
            Emit() << "Implements: " << imp;
        }
        for (const auto& i: c.second->Inputs) {
            Emit() << "Input " << i.first
                   << ", " << i.second->Id
                   << ", " << i.second->Name
                   << ", " << i.second->Type
                   << ", " << i.second->Order
                   << ", " << i.second->ProgramClass->Name
                   << ", " << i.second->Codec->GetName()
                   << (i.second->ReadOnly ? ", read only" : "");
        }
        for (const auto& i: c.second->Outputs) {
            Emit() << "Output " << i.first
                   << ", " << i.second->Id
                   << ", " << i.second->Name
                   << ", " << i.second->Type
                   << ", " << i.second->Order
                   << ", " << i.second->ProgramClass->Name
                   << ", " << i.second->Codec->GetName()
                   << (i.second->ReadOnly ? ", read only" : "");
        }
        for (const auto& i: c.second->Parameters) {
            Emit() << "Param " << i.first
                   << ", " << i.second->Id
                   << ", " << i.second->Name
                   << ", " << i.second->Type
                   << ", " << i.second->Order
                   << ", " << i.second->ProgramClass->Name
                   << ", " << i.second->Codec->GetName()
                   << (i.second->ReadOnly ? ", read only" : "");
        }
    }
}

TEST_F(TLoadConfigTest, SmartWebToMqttConfigWoDat) {
    auto classJson = WBMQTT::JSON::Parse(TestRootDir + "/classes/ROOM_DEVICE.json");
    TSmartWebToMqttConfig config;
    LoadSmartWebClass(config, classJson);
    ASSERT_LE(1, config.Classes.size());
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

TEST_F(TLoadConfigTest, SmartWebToMqttConfigInputs) {
    auto config = GetTestConfig();
    const auto smartWebClass = config->Classes.begin()->second;

    EXPECT_EQ(7 ,smartWebClass->Inputs.size());
    auto input = smartWebClass->Inputs.at(2);

    TestClassParameterSample sample = {
        .id = 2,
        .name = "floorT",
        .type = "temperature",
        .order = 2,
        .programClassName = "ROOM_DEVICE",
        .codecName = "TSensorCodec",
        .readOnly = true
    };

    ParameterEqHelper(sample, *input);
}

TEST_F(TLoadConfigTest, SmartWebToMqttConfigOutputs) {
    auto config = GetTestConfig();
    const auto smartWebClass = config->Classes.begin()->second;

    EXPECT_EQ(7 ,smartWebClass->Outputs.size());
    auto output = smartWebClass->Outputs.at(2);

    TestClassParameterSample sample = {
        .id = 2,
        .name = "addValve",
        .type = "relay",
        .order = 9,
        .programClassName = "ROOM_DEVICE",
        .codecName = "TOutputCodec",
        .readOnly = true
    };

    ParameterEqHelper(sample, *output);
}

TEST_F(TLoadConfigTest, SmartWebToMqttConfigParameters) {
    auto config = GetTestConfig();
    const auto smartWebClass = config->Classes.begin()->second;

    EXPECT_EQ(33 ,smartWebClass->Parameters.size());
    auto parameter = smartWebClass->Parameters.at(2);

    TestClassParameterSample sample = {
        .id = 2,
        .name = "roomReducedTemperature",
        .type = "temperature",
        .order = 16,
        .programClassName = "ROOM_DEVICE",
        .codecName = "TIntCodec<signed 2, 10>",
        .readOnly = false
    };

    ParameterEqHelper(sample, *parameter);
}