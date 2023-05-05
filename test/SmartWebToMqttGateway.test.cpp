#include "config_parser.h"

#include <gtest/gtest.h>
#include <vector>

#include <wblib/json_utils.h>
#include <wblib/testing/fake_driver.h>
#include <wblib/testing/fake_mqtt.h>
#include <wblib/testing/testlog.h>

using namespace WBMQTT;

class TSmartWebToMqttGatewayTest: public testing::Test
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

TEST_F(TSmartWebToMqttGatewayTest, MakeSetParameterValueRequest)
{
    TSmartWebClass cl;
    cl.Type = 5;
    cl.Name = "ROOM_DEVICE";

    TSmartWebParameter param;
    param.Id = 1;
    param.Name = "Test sensor";
    param.Order = 1;
    param.ProgramClass = &cl;
    param.ReadOnly = true;
    param.Type = "temperature";
    param.Codec = std::make_unique<TIntCodec<int16_t, 10>>();

    TSmartWebParameterControl pc;
    pc.ProgramId = 10;
    pc.Parameter = &param;

    auto frame = MakeSetParameterValueRequest(pc, "11.1");

    ASSERT_EQ(frame.can_id, 0x80020A16);
    ASSERT_EQ(frame.can_dlc, 4);
    ASSERT_EQ(frame.data[0], 5); // cl.Type
    ASSERT_EQ(frame.data[1], 1); // param.Id
    ASSERT_EQ(frame.data[2], 111);
    ASSERT_EQ(frame.data[3], 0);
}

TEST_F(TSmartWebToMqttGatewayTest, AddRequests)
{
    std::vector<CAN::TFrame> requests;
    TSmartWebClass cl;
    cl.Type = 5;
    cl.Name = "ROOM_DEVICE";
    cl.ParentClasses.push_back("TEST PARENT CLASS");
    cl.ParentClasses.push_back("MISSING TEST PARENT CLASS");

    auto inp = std::make_shared<TSmartWebParameter>();
    inp->Id = 1;
    inp->Name = "Test sensor";
    inp->Order = 1;
    inp->ProgramClass = &cl;
    inp->ReadOnly = true;
    inp->Type = "temperature";
    inp->Codec = std::make_unique<TIntCodec<int16_t, 10>>();
    cl.Inputs.insert({1, inp});

    auto out = std::make_shared<TSmartWebParameter>();
    out->Id = 2;
    out->Name = "Test out";
    out->Order = 1;
    out->ProgramClass = &cl;
    out->ReadOnly = true;
    out->Type = "PWM";
    out->Codec = std::make_unique<TIntCodec<uint8_t, 1>>();
    cl.Outputs.insert({2, out});

    auto param = std::make_shared<TSmartWebParameter>();
    param->Id = 3;
    param->Name = "Test param";
    param->Order = 1;
    param->ProgramClass = &cl;
    param->ReadOnly = false;
    param->Type = "humidity";
    param->Codec = std::make_unique<TIntCodec<int16_t, 10>>();
    cl.Parameters.insert({3, param});

    auto cl2 = std::make_shared<TSmartWebClass>();
    cl2->Type = 3;
    cl2->Name = "TEST PARENT CLASS";

    auto param2 = std::make_shared<TSmartWebParameter>();
    param->Id = 4;
    param->Name = "Test param2";
    param->Order = 5;
    param->ProgramClass = cl2.get();
    param->ReadOnly = true;
    param->Type = "temperature";
    param->Codec = std::make_unique<TIntCodec<uint16_t, 100>>();
    cl2->Parameters.insert({4, param});

    std::unordered_map<uint8_t, std::shared_ptr<TSmartWebClass>> classes;
    classes.insert({4, cl2});

    AddRequests(requests, "test", cl, 10, classes);

    ASSERT_EQ(requests.size(), 4);

    ASSERT_EQ(requests[0].can_id, 0x80010A16);
    ASSERT_EQ(requests[0].can_dlc, 3);
    ASSERT_EQ(requests[0].data[0], 1); // PT_PROGRAM
    ASSERT_EQ(requests[0].data[1], 1); // SmartWeb::RemoteControl::Parameters::SENSOR
    ASSERT_EQ(requests[0].data[2], 1); // id

    ASSERT_EQ(requests[1].can_id, 0x80010A16);
    ASSERT_EQ(requests[1].can_dlc, 3);
    ASSERT_EQ(requests[1].data[0], 1); // PT_PROGRAM
    ASSERT_EQ(requests[1].data[1], 2); // SmartWeb::RemoteControl::Parameters::OUTPUT
    ASSERT_EQ(requests[1].data[2], 2); // id

    ASSERT_EQ(requests[2].can_id, 0x80010A16);
    ASSERT_EQ(requests[2].can_dlc, 2);
    ASSERT_EQ(requests[2].data[0], 5); // cl.Type
    ASSERT_EQ(requests[2].data[1], 3);

    ASSERT_EQ(requests[3].can_id, 0x80010A16);
    ASSERT_EQ(requests[3].can_dlc, 2);
    ASSERT_EQ(requests[3].data[0], 3); // cl2->Type
    ASSERT_EQ(requests[3].data[1], 4);
}
