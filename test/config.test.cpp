#include "config_parser.h"

#include <gtest/gtest.h>
#include <vector>

#include <wblib/testing/testlog.h>
#include <wblib/testing/fake_driver.h>
#include <wblib/testing/fake_mqtt.h>
#include <wblib/json_utils.h>

using namespace WBMQTT;

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