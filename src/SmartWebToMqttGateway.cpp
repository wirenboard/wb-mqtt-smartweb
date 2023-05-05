#include "SmartWebToMqttGateway.h"

#include <string.h>
#include <wblib/exceptions.h>

#include "exceptions.h"
#include "log.h"

#include "MqttToSmartWebGateway.h"

namespace
{
    const int16_t SENSOR_SHORT_VALUE = -32768;
    const int16_t SENSOR_OPEN_VALUE = -32767;
    const int16_t SENSOR_UNDEFINED = -32766;
}

TEnumCodec::TEnumCodec(const std::map<uint8_t, std::string>& values): Values(values)
{
    for (const auto& v: values) {
        Keys.insert({v.second, v.first});
    }
}

std::string TEnumCodec::Decode(const uint8_t* buf) const
{
    auto it = Values.find(*buf);
    if (it != Values.end()) {
        return it->second;
    }
    return std::to_string((int)*buf);
}

std::vector<uint8_t> TEnumCodec::Encode(const std::string& value) const
{
    auto it = Keys.find(value);
    if (it != Keys.end()) {
        return {it->second};
    }
    throw std::runtime_error("unknown value for enum parameter: " + value);
}

std::string TEnumCodec::GetName() const
{
    std::string res;
    for (const auto& v: Values) {
        res += (res.empty() ? "" : ", ") + std::to_string(v.first) + ": " + v.second;
    }
    return "TEnumCodec (" + res + ")";
}

std::string TSensorCodec::Decode(const uint8_t* buf) const
{
    int16_t v;
    memcpy(&v, buf, 2);
    if (v == SENSOR_SHORT_VALUE || v == SENSOR_OPEN_VALUE || v == SENSOR_UNDEFINED) {
        throw std::runtime_error("sensor error " + std::to_string(v));
    }
    return WBMQTT::FormatFloat(v / 10.0);
}

std::vector<uint8_t> TSensorCodec::Encode(const std::string& value) const
{
    throw std::runtime_error("sensors are readonly");
}

std::string TSensorCodec::GetName() const
{
    return "TSensorCodec";
}

std::string TOnOffSensorCodec::Decode(const uint8_t* buf) const
{
    int16_t v;
    memcpy(&v, buf, 2);
    if (v == SENSOR_SHORT_VALUE) {
        return "1";
    }
    if (v == SENSOR_OPEN_VALUE) {
        return "0";
    }
    if (v == SENSOR_UNDEFINED) {
        throw std::runtime_error("sensor is in undefined state");
    }
    return std::to_string(v);
}

std::vector<uint8_t> TOnOffSensorCodec::Encode(const std::string& value) const
{
    std::vector<uint8_t> res;
    res.push_back(value == "0" ? 0 : 1);
    return res;
}

std::string TOnOffSensorCodec::GetName() const
{
    return "TOnOffSensorCodec";
}

std::string TPwmCodec::Decode(const uint8_t* buf) const
{
    if (buf[0] == 255) {
        return "100";
    }
    return WBMQTT::FormatFloat(buf[0] / 2.54);
}

std::vector<uint8_t> TPwmCodec::Encode(const std::string& value) const
{
    throw std::runtime_error("outputs are readonly");
}

std::string TPwmCodec::GetName() const
{
    return "TPwmCodec";
}

std::string TOutputCodec::Decode(const uint8_t* buf) const
{
    return (buf[0] == 0 ? "0" : "1");
}

std::vector<uint8_t> TOutputCodec::Encode(const std::string& value) const
{
    throw std::runtime_error("outputs are readonly");
}

std::string TOutputCodec::GetName() const
{
    return "TOutputCodec";
}

TSmartWebToMqttGateway::TSmartWebToMqttGateway(const TSmartWebToMqttConfig& config,
                                               std::shared_ptr<CAN::IPort> canPort,
                                               WBMQTT::PDeviceDriver driver)
    : CanPort(canPort),
      Config(config),
      Driver(driver),
      RequestIndex(0),
      Scheduler(MakeSimpleThreadedScheduler("SW to MQTT"))
{
    EventHandler = Driver->On<WBMQTT::TControlOnValueEvent>([this](const WBMQTT::TControlOnValueEvent& event) {
        try {
            auto param = event.Control->GetUserData().As<TSmartWebParameterControl>();
            auto frame = MakeSetParameterValueRequest(param, event.RawValue);
            CanPort->Send(frame);
            print_frame(DebugSwToMqtt, frame, "Set value request");
        } catch (const std::exception& e) {
            ErrorSwToMqtt.Log() << "Set value request: " << e.what();
        }
    });
    Scheduler->AddTask(MakePeriodicTask(
        config.PollInterval,
        [this]() { this->HandleMapping(); },
        "SmartWeb->MQTT task"));
    CanPort->AddHandler(this);
}

TSmartWebToMqttGateway::~TSmartWebToMqttGateway()
{
    Scheduler.reset();
    CanPort->RemoveHandler(this);
    Driver->RemoveEventHandler(EventHandler);
    auto tx = Driver->BeginTx();
    for (const auto& d: DeviceIds) {
        tx->RemoveDeviceById(d).Sync();
    }
}

bool TSmartWebToMqttGateway::Handle(const CAN::TFrame& frame)
{
    if (!(frame.can_id & CAN_EFF_FLAG)) {
        return false;
    }

    SmartWeb::TCanHeader* header = (SmartWeb::TCanHeader*)&frame.can_id;
    if (header->rec.message_type != SmartWeb::MT_MSG_RESPONSE) {
        return false;
    }

    if (header->rec.program_type == SmartWeb::PT_PROGRAM &&
        header->rec.function_id == SmartWeb::Program::Function::I_AM_PROGRAM)
    {
        AddProgram(frame);
        return true;
    }

    if (header->rec.program_type == SmartWeb::PT_REMOTE_CONTROL &&
        header->rec.function_id == SmartWeb::RemoteControl::Function::GET_PARAMETER_VALUE)
    {
        HandleGetValueResponse(frame);
        return true;
    }

    return false;
}

void TSmartWebToMqttGateway::HandleMapping()
{
    CAN::TFrame frame;
    {
        std::unique_lock<std::mutex> lk(RequestMutex);
        if (Requests.empty()) {
            return;
        }
        if (RequestIndex == Requests.size()) {
            RequestIndex = 0;
        }
        frame = Requests[RequestIndex];
    }
    try {
        CanPort->Send(frame);
        print_frame(DebugSwToMqtt, frame, "Send request");
    } catch (const std::exception& e) {
        print_frame(ErrorSwToMqtt, frame, std::string("Send request: ") + e.what());
    }
    ++RequestIndex;
}

CAN::TFrame MakeSetParameterValueRequest(const TSmartWebParameterControl& param, const std::string& value)
{
    CAN::TFrame frame{0};
    SmartWeb::TParameterData pd;
    pd.program_type = param.Parameter->ProgramClass->Type;
    pd.parameter_id = param.Parameter->Id;
    try {
        auto bytes = param.Parameter->Codec->Encode(value);
        memcpy(pd.value, bytes.data(), bytes.size());
        frame.can_dlc = bytes.size() + 2;
        memcpy(frame.data, &pd.raw, frame.can_dlc);
    } catch (std::exception& e) {
        throw std::runtime_error("Can't encode '" + value + "' for '" + param.Parameter->ProgramClass->Name + "'(" +
                                 std::to_string(int(param.ProgramId)) + "):'" + param.Parameter->Name + "'");
    }
    SmartWeb::TCanHeader header{0};
    header.rec.program_type = SmartWeb::PT_REMOTE_CONTROL;
    header.rec.program_id = param.ProgramId;
    header.rec.function_id = SmartWeb::RemoteControl::Function::SET_PARAMETER_VALUE;
    header.rec.message_type = SmartWeb::MT_MSG_REQUEST;
    frame.can_id = header.raw | CAN_EFF_FLAG;
    return frame;
}

void AddRequests(std::vector<CAN::TFrame>& requests,
                 const std::string& programType,
                 const TSmartWebClass& cl,
                 uint8_t programId,
                 const std::unordered_map<uint8_t, std::shared_ptr<TSmartWebClass>>& classes)
{
    CAN::TFrame frame{0};
    SmartWeb::TCanHeader header{0};
    header.rec.program_type = SmartWeb::PT_REMOTE_CONTROL;
    header.rec.program_id = programId;
    header.rec.function_id = SmartWeb::RemoteControl::Function::GET_PARAMETER_VALUE;
    header.rec.message_type = SmartWeb::MT_MSG_REQUEST;
    frame.can_id = header.raw | CAN_EFF_FLAG;

    SmartWeb::TParameterData pd;
    pd.program_type = SmartWeb::PT_PROGRAM;
    pd.parameter_id = SmartWeb::RemoteControl::Parameters::SENSOR;
    frame.can_dlc = 3;
    for (const auto& i: cl.Inputs) {
        pd.indexed_parameter.index = i.first;
        memcpy(frame.data, &pd.raw, frame.can_dlc);
        requests.push_back(frame);
    }

    pd.program_type = SmartWeb::PT_PROGRAM;
    pd.parameter_id = SmartWeb::RemoteControl::Parameters::OUTPUT;
    for (const auto& o: cl.Outputs) {
        pd.indexed_parameter.index = o.first;
        memcpy(frame.data, &pd.raw, frame.can_dlc);
        requests.push_back(frame);
    }

    pd.program_type = cl.Type;
    frame.can_dlc = 2;
    for (const auto& p: cl.Parameters) {
        pd.parameter_id = p.first;
        memcpy(frame.data, &pd.raw, frame.can_dlc);
        requests.push_back(frame);
    }

    for (const auto& c: cl.ParentClasses) {
        bool ok = false;
        for (const auto& cl: classes) {
            if (cl.second->Name == c) {
                AddRequests(requests, programType, *cl.second, programId, classes);
                ok = true;
                break;
            }
        }
        if (!ok && c != "PROGRAM") {
            WarnSwToMqtt.Log() << "Unknown program type: '" << c << "'";
        }
    }
}

void TSmartWebToMqttGateway::AddProgram(const CAN::TFrame& frame)
{
    SmartWeb::TCanHeader* header = (SmartWeb::TCanHeader*)&frame.can_id;
    if (KnownPrograms.count(header->rec.program_id)) {
        return;
    }
    auto cl = Config.Classes.find(frame.data[2]);
    if (cl == Config.Classes.end()) {
        print_frame(DebugSwToMqtt, frame, "Unknown program type");
        return;
    }
    InfoSwToMqtt.Log() << "New program '" << cl->second->Name << "':" << (int)header->rec.program_id << " is found";
    KnownPrograms.insert({header->rec.program_id, cl->second.get()});
    std::unique_lock<std::mutex> lk(RequestMutex);
    AddRequests(Requests, cl->second->Name, *cl->second, header->rec.program_id, Config.Classes);
}

WBMQTT::TControlArgs TSmartWebToMqttGateway::MakeControlArgs(uint8_t programId,
                                                             const TSmartWebParameter& param,
                                                             const std::string& value,
                                                             bool error)
{
    const std::unordered_map<std::string, std::string> types({{"temperature", "temperature"},
                                                              {"humidity", "rel_humidity"},
                                                              {"onOff", "switch"},
                                                              {"relay", "switch"},
                                                              {"PWM", "range"},
                                                              {"%", "range"},
                                                              {"id", "text"},
                                                              {"picklist", "text"}});

    WBMQTT::TControlArgs res;
    res.SetId(param.Name);
    res.SetOrder(param.Order);
    res.SetReadonly(param.ReadOnly);
    auto t = types.find(param.Type);
    res.SetType((t != types.end()) ? t->second : "value");
    if (!param.ReadOnly) {
        TSmartWebParameterControl pc;
        pc.ProgramId = programId;
        pc.Parameter = &param;
        res.SetUserData(pc);
    }
    if (param.Type == "PWM" || param.Type == "%") {
        res.SetMax(100);
        res.SetUnits("%");
    }
    if (param.Type == "minutes") {
        res.SetUnits("min");
    }
    if (error) {
        res.SetError("r");
        res.SetRawValue(value.empty() ? "0" : value);
    } else {
        res.SetRawValue(value);
    }
    return res;
}

void TSmartWebToMqttGateway::SetParameter(const std::map<uint32_t, std::shared_ptr<TSmartWebParameter>>& params,
                                          uint8_t parameterId,
                                          const uint8_t* data,
                                          uint8_t programId)
{
    auto p = params.find(parameterId);
    if (p == params.end()) {
        DebugSwToMqtt.Log() << "Unknown parameter id: " << (int)parameterId;
        return;
    }
    std::string res;
    bool error = false;
    try {
        res = p->second->Codec->Decode(data);
    } catch (const std::exception& e) {
        WarnSwToMqtt.Log() << "Error reading '" << p->second->ProgramClass->Name << "':" << (int)programId << " "
                           << p->second->Name << ": " << e.what();
        error = true;
    }
    std::string deviceName("sw " + p->second->ProgramClass->Name + " " + std::to_string(programId));
    try {
        auto tx = Driver->BeginTx();
        WBMQTT::PLocalDevice device(std::dynamic_pointer_cast<WBMQTT::TLocalDevice>(tx->GetDevice(deviceName)));
        if (!device) {
            device =
                tx->CreateDevice(WBMQTT::TLocalDeviceArgs{}.SetId(deviceName).SetTitle(deviceName).SetIsVirtual(true))
                    .GetValue();
            DeviceIds.push_back(device->GetId());
        }
        auto control = device->GetControl(p->second->Name);
        if (control) {
            if (error) {
                control->SetError(tx, "r").Sync();
            } else {
                control->SetRawValue(tx, res).Sync();
            }
        } else {
            device->CreateControl(tx, MakeControlArgs(programId, *p->second, res, error)).GetValue();
        }
    } catch (const std::exception& e) {
        ErrorSwToMqtt.Log() << e.what();
    }
}

void TSmartWebToMqttGateway::HandleGetValueResponse(const CAN::TFrame& frame)
{
    SmartWeb::TCanHeader* header = (SmartWeb::TCanHeader*)&frame.can_id;
    auto cl = KnownPrograms.find(header->rec.program_id);
    if (cl == KnownPrograms.end()) {
        return;
    }

    print_frame(DebugSwToMqtt, frame, "Get value response");

    SmartWeb::TParameterData* data = (SmartWeb::TParameterData*)&frame.data;
    if (data->program_type == SmartWeb::PT_PROGRAM) {
        if (data->parameter_id == SmartWeb::RemoteControl::Parameters::SENSOR) {
            SetParameter(cl->second->Inputs,
                         data->indexed_parameter.index,
                         data->indexed_parameter.value,
                         header->rec.program_id);
            return;
        }
        if (data->parameter_id == SmartWeb::RemoteControl::Parameters::OUTPUT) {
            SetParameter(cl->second->Outputs,
                         data->indexed_parameter.index,
                         data->indexed_parameter.value,
                         header->rec.program_id);
            return;
        }
        DebugSwToMqtt.Log() << "Unknown parameter id: " << (int)data->parameter_id;
        return;
    }

    auto clParam = Config.Classes.find(data->program_type);
    if (clParam == Config.Classes.end()) {
        DebugSwToMqtt.Log() << "Unknown program type: " << (int)data->program_type;
        return;
    }
    SetParameter(clParam->second->Parameters, data->parameter_id, data->value, header->rec.program_id);
}