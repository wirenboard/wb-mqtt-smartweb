#include "config_parser.h"

#include <gtest/gtest.h>
#include <vector>

#include <wblib/testing/testlog.h>
#include <wblib/testing/fake_driver.h>
#include <wblib/testing/fake_mqtt.h>
#include <wblib/json_utils.h>

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

}

TEST_F(TSmartWebToMqttGatewayTest, AddRequests)
{

}