#include "SmartWebToMqttGateway.h"
#include "smart_web_conventions.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>
#include <wblib/driver.h>
#include <wblib/testing/fake_mqtt.h>
#include <wblib/testing/testlog.h>

using namespace WBMQTT;

namespace
{
    const uint8_t PROGRAM_ID = 10;
    const uint8_t CLASS_TYPE = 5;
    const uint32_t SENSOR_ID = 1;
    const uint32_t PARAM_ID = 3;
    const std::string DEVICE_NAME = "sw ROOM_DEVICE 10";
    const std::string SENSOR_CONTROL = "Test sensor";
    const std::string PARAM_CONTROL = "Test param";
    const auto POLL_INTERVAL = std::chrono::milliseconds(20);
    const auto WAIT_TIMEOUT = std::chrono::seconds(3);

    class TFakeCanPort: public CAN::IPort
    {
        std::mutex Mutex;
        std::vector<CAN::IFrameHandler*> Handlers;
        bool Connected = true;
        bool FailWrites = false;
        bool FailAll = false;

    public:
        void AddHandler(CAN::IFrameHandler* handler) override
        {
            std::unique_lock<std::mutex> lk(Mutex);
            Handlers.push_back(handler);
            handler->OnConnectionChanged(Connected);
        }

        void RemoveHandler(CAN::IFrameHandler* handler) override
        {
            std::unique_lock<std::mutex> lk(Mutex);
            Handlers.erase(std::remove(Handlers.begin(), Handlers.end(), handler), Handlers.end());
        }

        void Send(const CAN::TFrame& frame) override
        {
            std::unique_lock<std::mutex> lk(Mutex);
            if (!Connected) {
                throw std::runtime_error("CAN port is disconnected");
            }
            if (FailAll) {
                throw std::runtime_error("CAN write error");
            }
            SmartWeb::TCanHeader header;
            header.raw = frame.can_id & CAN_EFF_MASK;
            if (FailWrites && header.rec.function_id == SmartWeb::RemoteControl::Function::SET_PARAMETER_VALUE) {
                throw std::runtime_error("CAN write timeout");
            }
        }

        void SetConnected(bool connected)
        {
            std::unique_lock<std::mutex> lk(Mutex);
            Connected = connected;
            for (auto handler: Handlers) {
                handler->OnConnectionChanged(connected);
            }
        }

        void SetFailWrites(bool fail)
        {
            std::unique_lock<std::mutex> lk(Mutex);
            FailWrites = fail;
        }

        void SetFailAll(bool fail)
        {
            std::unique_lock<std::mutex> lk(Mutex);
            FailAll = fail;
        }

        void Receive(const CAN::TFrame& frame)
        {
            std::vector<CAN::IFrameHandler*> handlers;
            {
                std::unique_lock<std::mutex> lk(Mutex);
                handlers = Handlers;
            }
            for (auto handler: handlers) {
                handler->Handle(frame);
            }
        }
    };

    CAN::TFrame MakeResponseHeader(uint8_t programType, uint8_t functionId)
    {
        CAN::TFrame frame{0};
        SmartWeb::TCanHeader header{0};
        header.rec.program_type = programType;
        header.rec.program_id = PROGRAM_ID;
        header.rec.function_id = functionId;
        header.rec.message_type = SmartWeb::MT_MSG_RESPONSE;
        frame.can_id = header.raw | CAN_EFF_FLAG;
        return frame;
    }

    CAN::TFrame MakeIAmProgramFrame()
    {
        auto frame = MakeResponseHeader(SmartWeb::PT_PROGRAM, SmartWeb::Program::Function::I_AM_PROGRAM);
        frame.can_dlc = 3;
        frame.data[2] = CLASS_TYPE;
        return frame;
    }

    CAN::TFrame MakeParameterValueFrame(int16_t value)
    {
        auto frame =
            MakeResponseHeader(SmartWeb::PT_REMOTE_CONTROL, SmartWeb::RemoteControl::Function::GET_PARAMETER_VALUE);
        SmartWeb::TParameterData pd{0};
        pd.program_type = CLASS_TYPE;
        pd.parameter_id = PARAM_ID;
        memcpy(pd.value, &value, sizeof(value));
        frame.can_dlc = 4;
        memcpy(frame.data, &pd.raw, frame.can_dlc);
        return frame;
    }

    CAN::TFrame MakeSensorValueFrame(int16_t value)
    {
        auto frame =
            MakeResponseHeader(SmartWeb::PT_REMOTE_CONTROL, SmartWeb::RemoteControl::Function::GET_PARAMETER_VALUE);
        SmartWeb::TParameterData pd{0};
        pd.program_type = SmartWeb::PT_PROGRAM;
        pd.parameter_id = SmartWeb::RemoteControl::Parameters::SENSOR;
        pd.indexed_parameter.index = SENSOR_ID;
        memcpy(pd.indexed_parameter.value, &value, sizeof(value));
        frame.can_dlc = 5;
        memcpy(frame.data, &pd.raw, frame.can_dlc);
        return frame;
    }
}

class TSmartWebToMqttGatewayErrorsTest: public Testing::TLoggedFixture
{
protected:
    Testing::PFakeMqttBroker MqttBroker;
    Testing::PFakeMqttClient MqttClient;
    Testing::PFakeMqttClient Observer;
    std::mutex DeviceErrorMutex;
    std::string DeviceError = "<none>";
    PDeviceDriver Driver;
    std::shared_ptr<TFakeCanPort> Port;
    TSmartWebToMqttConfig Config;
    std::unique_ptr<TSmartWebToMqttGateway> Gateway;

    void SetUp() override
    {
        TLoggedFixture::SetUp();

        MqttBroker = Testing::NewFakeMqttBroker(*this);
        MqttClient = MqttBroker->MakeClient("test");
        Observer = MqttBroker->MakeClient("observer");
        Observer->Start();
        Observer->Subscribe(
            [this](const TMqttMessage& message) {
                std::unique_lock<std::mutex> lk(DeviceErrorMutex);
                DeviceError = message.Payload;
            },
            "/devices/" + DEVICE_NAME + "/meta/error");
        Driver = NewDriver(TDriverArgs{}
                               .SetId("test")
                               .SetBackend(NewDriverBackend(MqttClient))
                               .SetIsTesting(true)
                               .SetUseStorage(false)
                               .SetReownUnknownDevices(false));
        Driver->StartLoop();

        auto cl = std::make_shared<TSmartWebClass>();
        cl->Type = CLASS_TYPE;
        cl->Name = "ROOM_DEVICE";

        auto sensor = std::make_shared<TSmartWebParameter>();
        sensor->Id = SENSOR_ID;
        sensor->Name = SENSOR_CONTROL;
        sensor->Order = 1;
        sensor->ProgramClass = cl.get();
        sensor->ReadOnly = true;
        sensor->Type = "temperature";
        sensor->Codec = std::make_unique<TSensorCodec>();
        cl->Inputs.insert({SENSOR_ID, sensor});

        auto param = std::make_shared<TSmartWebParameter>();
        param->Id = PARAM_ID;
        param->Name = PARAM_CONTROL;
        param->Order = 2;
        param->ProgramClass = cl.get();
        param->ReadOnly = false;
        param->Type = "temperature";
        param->Codec = std::make_unique<TIntCodec<int16_t, 10>>();
        cl->Parameters.insert({PARAM_ID, param});

        Config.Classes.insert({CLASS_TYPE, cl});
        Port = std::make_shared<TFakeCanPort>();
    }

    void StartGateway(std::chrono::milliseconds pollInterval = std::chrono::hours(1))
    {
        Config.PollInterval = pollInterval;
        Gateway = std::make_unique<TSmartWebToMqttGateway>(Config, Port, Driver);
    }

    void TearDown() override
    {
        Gateway.reset();
        Driver->StopLoop();
        Driver->Close();
    }

    std::string GetControlError(const std::string& controlId)
    {
        auto tx = Driver->BeginTx();
        auto device = tx->GetDevice(DEVICE_NAME);
        if (!device) {
            return "<no device>";
        }
        auto control = device->GetControl(controlId);
        if (!control) {
            return "<no control>";
        }
        return control->GetError();
    }

    bool WaitForDeviceError(const std::string& error)
    {
        auto deadline = std::chrono::steady_clock::now() + WAIT_TIMEOUT;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::unique_lock<std::mutex> lk(DeviceErrorMutex);
                if (DeviceError == error) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    bool WaitForControlError(const std::string& controlId, const std::string& error)
    {
        auto deadline = std::chrono::steady_clock::now() + WAIT_TIMEOUT;
        while (std::chrono::steady_clock::now() < deadline) {
            if (GetControlError(controlId) == error) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    void DiscoverDevice()
    {
        Port->Receive(MakeIAmProgramFrame());
        Port->Receive(MakeParameterValueFrame(215));
        Port->Receive(MakeSensorValueFrame(230));
        ASSERT_TRUE(WaitForControlError(PARAM_CONTROL, ""));
        ASSERT_TRUE(WaitForControlError(SENSOR_CONTROL, ""));
    }

    void WriteParameter(const std::string& value)
    {
        MqttBroker->Publish("test-publisher",
                            {TMqttMessage("/devices/" + DEVICE_NAME + "/controls/" + PARAM_CONTROL + "/on", value, 0)});
    }
};

TEST_F(TSmartWebToMqttGatewayErrorsTest, PortLossMarksControlsAndRestoreClears)
{
    StartGateway();
    DiscoverDevice();

    Port->SetConnected(false);
    ASSERT_TRUE(WaitForDeviceError("r"));
    ASSERT_TRUE(WaitForControlError(PARAM_CONTROL, "r"));
    ASSERT_TRUE(WaitForControlError(SENSOR_CONTROL, "r"));

    Port->SetConnected(true);
    ASSERT_TRUE(WaitForDeviceError(""));
    ASSERT_TRUE(WaitForControlError(PARAM_CONTROL, ""));
    ASSERT_TRUE(WaitForControlError(SENSOR_CONTROL, ""));
}

TEST_F(TSmartWebToMqttGatewayErrorsTest, FailedWriteMarksControlAndGoodReadClears)
{
    StartGateway();
    DiscoverDevice();

    Port->SetFailWrites(true);
    WriteParameter("22.5");
    ASSERT_TRUE(WaitForControlError(PARAM_CONTROL, "w"));
    ASSERT_EQ(GetControlError(SENSOR_CONTROL), "");

    Port->Receive(MakeParameterValueFrame(215));
    ASSERT_TRUE(WaitForControlError(PARAM_CONTROL, ""));
}

TEST_F(TSmartWebToMqttGatewayErrorsTest, FailedWriteWhilePortIsLostAppendsToReadError)
{
    StartGateway();
    DiscoverDevice();

    Port->SetConnected(false);
    ASSERT_TRUE(WaitForControlError(PARAM_CONTROL, "r"));

    WriteParameter("22.5");
    ASSERT_TRUE(WaitForControlError(PARAM_CONTROL, "rw"));
}

TEST_F(TSmartWebToMqttGatewayErrorsTest, DecodeErrorMarksControlAndGoodValueClears)
{
    StartGateway();
    DiscoverDevice();

    Port->Receive(MakeSensorValueFrame(SmartWeb::SENSOR_UNDEFINED));
    ASSERT_TRUE(WaitForControlError(SENSOR_CONTROL, "r"));
    ASSERT_EQ(GetControlError(PARAM_CONTROL), "");

    Port->Receive(MakeSensorValueFrame(230));
    ASSERT_TRUE(WaitForControlError(SENSOR_CONTROL, ""));
}

TEST_F(TSmartWebToMqttGatewayErrorsTest, FailedRequestMarksDeviceControlsAndResponseClears)
{
    StartGateway(POLL_INTERVAL);
    DiscoverDevice();

    Port->SetFailAll(true);
    ASSERT_TRUE(WaitForDeviceError("r"));
    ASSERT_TRUE(WaitForControlError(PARAM_CONTROL, "r"));
    ASSERT_TRUE(WaitForControlError(SENSOR_CONTROL, "r"));

    Port->SetFailAll(false);
    Port->Receive(MakeParameterValueFrame(215));
    ASSERT_TRUE(WaitForDeviceError(""));
    ASSERT_TRUE(WaitForControlError(PARAM_CONTROL, ""));
    ASSERT_TRUE(WaitForControlError(SENSOR_CONTROL, ""));
}

TEST_F(TSmartWebToMqttGatewayErrorsTest, PortRestoreKeepsOtherErrors)
{
    StartGateway();
    DiscoverDevice();

    Port->SetFailWrites(true);
    WriteParameter("22.5");
    ASSERT_TRUE(WaitForControlError(PARAM_CONTROL, "w"));
    Port->Receive(MakeSensorValueFrame(SmartWeb::SENSOR_UNDEFINED));
    ASSERT_TRUE(WaitForControlError(SENSOR_CONTROL, "r"));

    Port->SetConnected(false);
    ASSERT_TRUE(WaitForControlError(PARAM_CONTROL, "rw"));
    ASSERT_TRUE(WaitForControlError(SENSOR_CONTROL, "r"));

    Port->SetConnected(true);
    ASSERT_TRUE(WaitForControlError(PARAM_CONTROL, "w"));
    ASSERT_TRUE(WaitForControlError(SENSOR_CONTROL, "r"));

    Port->Receive(MakeParameterValueFrame(215));
    Port->Receive(MakeSensorValueFrame(230));
    ASSERT_TRUE(WaitForControlError(PARAM_CONTROL, ""));
    ASSERT_TRUE(WaitForControlError(SENSOR_CONTROL, ""));
}
