#include "MqttToSmartWebGateway.h"

#include <string.h>
#include <wblib/exceptions.h>

#include "exceptions.h"
#include "log.h"

using namespace WBMQTT;
using namespace CAN;
using namespace std;

namespace
{
    const auto KEEP_ALIVE_INTERVAL_S = TTimeIntervalS(10); // if nothing else to do - each 10 seconds send I_AM_HERE
    const auto CONNECTION_TIMEOUT_MIN =
        TTimeIntervalMin(10); // after 10 minutes without any messages connection is considered lost
    const auto SEND_MESSAGES_TIME_M = TTimeIntervalMin(10);       // send value during 10 minutes
    const auto SEND_MESSAGES_INTERVAL_MS = TTimeIntervalMs(1000); // interval between messages
    const auto READ_TIMEOUT_MS = TTimeIntervalMs(1000);           // 1 sec for messages waiting

    TTimePoint now()
    {
        return chrono::steady_clock::now();
    }
}

#define LOG(logger) ::logger.Log() << LOGGER_PREFIX

void print_frame(WBMQTT::TLogger& logger, const CAN::TFrame& frame, const std::string& prefix)
{
    if (logger.IsEnabled()) {
        SmartWeb::TCanHeader header;
        header.raw = frame.can_id;

        std::stringstream ss;
        for (int i = 0; i < frame.can_dlc; ++i) {
            ss << " " << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)frame.data[i];
        }
        logger.Log() << prefix << ": " << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
                     << frame.can_id << " (pt: " << std::dec << (int)header.rec.program_type << ", pid: " << std::dec
                     << (int)header.rec.program_id << ", fid: " << std::dec << (int)header.rec.function_id
                     << ", mf: " << std::dec << (int)header.rec.message_format << ", mt: " << std::dec
                     << (int)header.rec.message_type << ")" << ss.str();
    }
}

void TMqttChannel::from_string(const string& deviceControl)
{
    auto delimiter_position = deviceControl.find('/');

    if (delimiter_position == string::npos) {
        throw TDriverError("unable to determine device id and control id from string '" + deviceControl + "'");
    }

    device = deviceControl.substr(0, delimiter_position);
    control = deviceControl.substr(delimiter_position + 1);

    if (device.empty() || control.empty()) {
        throw TDriverError("unable to determine device id or control id from string '" + deviceControl + "'");
    }
}

bool TMqttChannel::is_initialized() const
{
    return !device.empty() || !control.empty();
}

string TMqttChannel::to_string(const string& device, const string& control)
{
    return device + "/" + control;
}

string TMqttChannel::to_string() const
{
    return to_string(device, control);
}

void TChannelState::postpone_send()
{
    SendTimePoint = now() + SEND_MESSAGES_INTERVAL_MS;
}

void TChannelState::postpone_send_end()
{
    SendEndTimePoint = now() + SEND_MESSAGES_TIME_M;
}

void TChannelState::schedule_to_send()
{
    SendTimePoint = now();
    postpone_send_end();
}

void TBroadcastChannel::schedule_to_send(const SmartWeb::TMappingPoint& mp)
{
    TChannelState::schedule_to_send();
    mapping_point = mp;
}

TMqttChannelTiming::TMqttChannelTiming(const TMqttChannelTiming& other)
    : LastUpdateTimePointMutex(),
      ValueTimeoutMin(other.ValueTimeoutMin)
{
    unique_lock<mutex> lock(other.LastUpdateTimePointMutex);
    LastUpdateTimePoint = other.LastUpdateTimePoint;
}

void TMqttChannelTiming::refresh_last_update_timepoint()
{
    unique_lock<mutex> lock(LastUpdateTimePointMutex);
    LastUpdateTimePoint = now();
}

bool TMqttChannelTiming::is_timed_out() const
{
    if (ValueTimeoutMin.count() < 0) {
        return false;
    }

    unique_lock<mutex> lock(LastUpdateTimePointMutex);
    return (now() - LastUpdateTimePoint) > ValueTimeoutMin;
}

bool TMqttToSmartWebGateway::FilterIsSet = false;
std::mutex TMqttToSmartWebGateway::StartupMutex;

TMqttToSmartWebGateway::TMqttToSmartWebGateway(const TMqttToSmartWebConfig& config,
                                               std::shared_ptr<CAN::IPort> canPort,
                                               WBMQTT::PDeviceDriver driver)
    : DriverState(config),
      CanPort(canPort),
      Driver(driver)
{
    CONTROLLER_TYPE = 14; // External controller
    Enabled.store(true);
    CanPort->AddHandler(this);
    Thread = std::thread([this]() { TaskFn(); });
}

TMqttToSmartWebGateway::~TMqttToSmartWebGateway()
{
    CanPort->RemoveHandler(this);
    Enabled.store(false);
    if (Thread.joinable()) {
        Thread.join();
    }
}

bool TMqttToSmartWebGateway::Handle(const CAN::TFrame& frame)
{
    SmartWeb::TCanHeader header;
    header.raw = frame.can_id;
    if (!IsForMe(header, frame.data)) {
        return false;
    }
    std::unique_lock<std::mutex> waitLock(CanFramesMutex);
    CanFrames.push(frame);
    waitLock.unlock();
    CanFramesCv.notify_all();
    return true;
}

bool TMqttToSmartWebGateway::SelectTimeout(CAN::TFrame& frame)
{
    std::unique_lock<std::mutex> waitLock(CanFramesMutex);
    if (CanFrames.empty()) {
        if (std::cv_status::timeout == CanFramesCv.wait_for(waitLock, READ_TIMEOUT_MS)) {
            return false;
        }
    }
    frame = CanFrames.front();
    CanFrames.pop();
    return true;
}

bool TMqttToSmartWebGateway::IsForMe(const SmartWeb::TCanHeader& header, const TFrameData& data) const
{
    if (header.rec.program_id == DriverState.ProgramId) {
        return true;
    }
    if (header.rec.message_type == SmartWeb::MT_MSG_REQUEST && header.rec.program_type == SmartWeb::PT_CONTROLLER &&
        header.rec.function_id == SmartWeb::Controller::Function::GET_OUTPUT_VALUE)
    {
        SmartWeb::TMappingPoint mapping_point;
        memcpy(&mapping_point.raw, data, 2);
        return mapping_point.hostID == DriverState.ProgramId;
    }

    return false;
}

void TMqttToSmartWebGateway::TaskFn()
{
    WBMQTT::SetThreadName("MQTT to SW " + to_string(int(DriverState.ProgramId)));
    {
        std::unique_lock<std::mutex> lk(StartupMutex);
        if (!FilterIsSet) {
            Driver->SetFilter(GetAllDevicesFilter());
            Driver->WaitForReady();
            FilterIsSet = true;
        }
    }

    uint8_t program_id = DriverState.ProgramId;

    auto onHandler = Driver->On<TControlValueEvent>([&](const TControlValueEvent& event) {
        auto deviceControl = TMqttChannel::to_string(event.Control->GetDevice()->GetId(), event.Control->GetId());
        try {
            DriverState.MqttChannelsTiming.at(deviceControl).refresh_last_update_timepoint();
        } catch (out_of_range&) {
        }
    });

    auto get_response_frame = [&](SmartWeb::TCanHeader header) {
        if (header.rec.message_type != SmartWeb::MT_MSG_REQUEST) {
            throw TFrameError("Frame error: response to NOT request frame");
        }

        if (header.rec.program_id != program_id) {
            throw TFrameError("Frame error: response to request frame for different device (" +
                              to_string((int)header.rec.program_id) + ")");
        }

        header.rec.message_type = SmartWeb::MT_MSG_RESPONSE;

        TFrame response{0};
        response.can_id = header.raw;

        return response;
    };

    auto postpone_i_am_here = [&] { SendIAmHereTime = now() + KEEP_ALIVE_INTERVAL_S; };
    auto postpone_connection_reset = [&] { ResetConnectionTime = now() + CONNECTION_TIMEOUT_MIN; };

    auto send_frame = [&](TFrame& frame, const std::string& prefix) {
        frame.can_id |= CAN_EFF_FLAG; // just in case
        try {
            CanPort->Send(frame);
            print_frame(DebugMqttToSw, frame, "[" + std::to_string(DriverState.ProgramId) + "] " + prefix);
        } catch (const std::exception& e) {
            print_frame(ErrorMqttToSw,
                        frame,
                        "[" + std::to_string(DriverState.ProgramId) + "] " + prefix + " " + e.what());
        }
    };

    auto read_mqtt_value = [&](const string& device_id, const string& control_id) {
        try {
            const auto& mqtt_channel_timing =
                DriverState.MqttChannelsTiming.at(TMqttChannel::to_string(device_id, control_id));
            if (mqtt_channel_timing.is_timed_out()) {
                WarnMqttToSw.Log() << "MQTT value of control " << control_id << " of device " << device_id
                                   << " timed out. Returning undefined value";
                return SmartWeb::SENSOR_UNDEFINED;
            }
        } catch (out_of_range&) {
            // Should never happen. Means that we did not add all mqtt channels to DriverState.MqttChannelsTiming at
            // startup as we should've.
            ErrorMqttToSw.Log() << "[error code 1] There is bug in code; Report error code to driver maintainer";
        }

        try {
            auto tx = Driver->BeginTx();
            if (auto device = tx->GetDevice(device_id)) {
                if (auto control = device->GetControl(control_id)) {
                    if (control->GetError().empty()) {
                        return SmartWeb::SensorData::FromDouble(control->GetValue().As<double>());
                    } else {
                        WarnMqttToSw.Log() << "Unable to read mqtt value because of error on control " << control_id
                                           << " of device " << device_id << ": " << control->GetError();
                        return SmartWeb::SENSOR_UNDEFINED;
                    }
                } else {
                    WarnMqttToSw.Log() << "Unable to read mqtt value because control " << control_id << " of device "
                                       << device_id << " does not exist";
                    return SmartWeb::SENSOR_UNDEFINED;
                }
            } else {
                WarnMqttToSw.Log() << "Unable to read mqtt value because device " << device_id << " does not exist";
                return SmartWeb::SENSOR_UNDEFINED;
            }
        } catch (const WBMQTT::TBaseException& e) {
            WarnMqttToSw.Log() << "Unable to read mqtt value: " << e.what();

            return SmartWeb::SENSOR_UNDEFINED;
        }
    };

    auto i_am_here = [&] {
        postpone_i_am_here();

        SmartWeb::TCanHeader header;

        header.rec.program_type = SmartWeb::PT_CONTROLLER;
        header.rec.program_id = program_id;
        header.rec.function_id = SmartWeb::Controller::Function::I_AM_HERE;
        header.rec.message_format = SmartWeb::MF_FORMAT_0;
        header.rec.message_type = SmartWeb::MT_MSG_RESPONSE;

        TFrame frame{0};
        frame.can_id = header.raw | CAN_EFF_FLAG;
        frame.can_dlc = 1;
        frame.data[0] = CONTROLLER_TYPE;

        send_frame(frame, "send I_AM_HERE");
    };

    auto send_scheduled_i_am_here = [&] {
        if (SendIAmHereTime <= now()) {
            i_am_here();
        }
    };

    auto get_channel_number = [&](const SmartWeb::TCanHeader& header) {
        auto channel_number = max((size_t)DriverState.ParameterCount, DriverState.ParameterMapping.size());

        auto response = get_response_frame(header);
        response.can_dlc = 2;
        response.data[0] = 0xFF & channel_number;
        response.data[1] = 0xFF & channel_number >> 8;
        send_frame(response, "send channel number");
    };

    auto get_parameter_value = [&](const SmartWeb::TCanHeader& header, const TFrameData& data) {
        SmartWeb::TParameterData parameter_data{0};

        memcpy(&parameter_data.raw, data, 4);

        if (parameter_data.program_type != SmartWeb::PT_CONTROLLER) {
            throw TUnsupportedError("Unsupported program type " + to_string((int)parameter_data.program_type) +
                                    " for GET_PARAMETER_VALUE");
        }

        const auto& itDeviceChannel = DriverState.ParameterMapping.find(parameter_data.raw_info);

        int16_t value = SmartWeb::SENSOR_UNDEFINED;

        if (itDeviceChannel == DriverState.ParameterMapping.end()) {
            WarnMqttToSw.Log() << "[" << (int)DriverState.ProgramId
                               << "] unmapped parameter: type: " << (int)parameter_data.program_type
                               << ", id: " << (int)parameter_data.parameter_id
                               << ", index: " << (int)parameter_data.indexed_parameter.index;
        } else {
            DebugMqttToSw.Log() << "[" << (int)DriverState.ProgramId
                                << "] get parameter: type: " << (int)parameter_data.program_type
                                << ", id: " << (int)parameter_data.parameter_id
                                << ", index: " << (int)parameter_data.indexed_parameter.index << ", raw "
                                << (int)parameter_data.raw_info;
            value = read_mqtt_value(itDeviceChannel->second.device, itDeviceChannel->second.control);
        }

        auto response = get_response_frame(header);

        response.can_dlc = 5;

        memset(response.data, 0, sizeof response.data);

        memcpy(parameter_data.indexed_parameter.value, &value, sizeof value);

        memcpy(response.data, &parameter_data.raw, 5);

        send_frame(response, "get parameter response");

        DebugMqttToSw.Log() << "[" << (int)DriverState.ProgramId
                            << "] parameter {type: " << (int)parameter_data.program_type
                            << ", id: " << (int)parameter_data.parameter_id
                            << ", index: " << (int)parameter_data.indexed_parameter.index
                            << "} <== " << SmartWeb::SensorData::ToDouble(value);
    };

    auto get_output_value = [&](const SmartWeb::TCanHeader& header, const TFrameData& data) {
        SmartWeb::TMappingPoint mapping_point{};
        memcpy(mapping_point.rawID, data, 2);

        if (mapping_point.hostID != program_id) {
            throw TFrameError("hostID of mapping point does not match with driver program_id");
        }

        auto channel_id = mapping_point.channelID;
        if (channel_id >= CONTROLLER_OUTPUT_MAX) {
            throw TFrameError("channel_id of mapping point is out of bounds: " + to_string(channel_id));
        }

        auto& channel = DriverState.OutputMapping[channel_id];

        if (channel.is_initialized()) {
            channel.schedule_to_send(mapping_point);
            InfoMqttToSw.Log() << "[" << (int)DriverState.ProgramId << "] scheduled output " << (int)channel_id;
        } else {
            WarnMqttToSw.Log() << "[" << (int)DriverState.ProgramId << "] unmapped output " << (int)channel_id;
        }
    };

    auto send_scheduled_outputs = [&] {
        SmartWeb::TCanHeader header;
        header.rec.program_type = SmartWeb::PT_CONTROLLER;
        header.rec.program_id = program_id;
        header.rec.function_id = SmartWeb::Controller::Function::GET_OUTPUT_VALUE;
        header.rec.message_format = SmartWeb::MF_FORMAT_0;
        header.rec.message_type = SmartWeb::MT_MSG_RESPONSE;

        TFrame frame{0};

        frame.can_dlc = 4;

        for (uint8_t channel_id = 0; channel_id < CONTROLLER_OUTPUT_MAX; ++channel_id) {
            auto& channel = DriverState.OutputMapping[channel_id];

            if (channel.SendEndTimePoint < now()) {
                continue; // too late
            }

            if (channel.SendTimePoint > now()) {
                continue; // too soon
            }

            if (channel.device.empty() || channel.control.empty()) {
                continue; // weird
            }

            auto value = read_mqtt_value(channel.device, channel.control);

            frame.can_id = header.raw | CAN_EFF_FLAG;
            memcpy(frame.data, &channel.mapping_point.raw, sizeof channel.mapping_point.raw);
            frame.data[2] = 0xFF & value >> 8;
            frame.data[3] = 0xFF & value;

            send_frame(frame, "send output");

            DebugMqttToSw.Log() << "[" << (int)DriverState.ProgramId << "] output {channel_id: " << (int)channel_id
                                << "} <== " << SmartWeb::SensorData::ToDouble(value);

            channel.postpone_send();
        }
    };

    auto get_controller_type = [&](const SmartWeb::TCanHeader& header) {
        auto response = get_response_frame(header);
        response.can_dlc = 1;
        response.data[0] = CONTROLLER_TYPE;
        send_frame(response, "send controller type");
    };

    auto handle_request = [&](const SmartWeb::TCanHeader& header, const TFrameData& data) {
        switch (header.rec.program_type) {
            case SmartWeb::PT_CONTROLLER:
                switch (header.rec.function_id) {
                    case SmartWeb::Controller::Function::GET_CHANNEL_NUMBER:
                        return get_channel_number(header);
                    case SmartWeb::Controller::Function::GET_CONTROLLER_TYPE:
                        return get_controller_type(header);
                    case SmartWeb::Controller::Function::GET_OUTPUT_VALUE:
                        return get_output_value(header, data);
                    case SmartWeb::Controller::Function::I_AM_HERE:
                        return i_am_here();
                    default:
                        throw TUnsupportedError("function id " + to_string((int)header.rec.function_id) +
                                                " is unsupported");
                }
            case SmartWeb::PT_REMOTE_CONTROL:
                switch (header.rec.function_id) {
                    case SmartWeb::RemoteControl::Function::GET_PARAMETER_VALUE:
                        return get_parameter_value(header, data);
                    default:
                        throw TUnsupportedError("function id " + to_string((int)header.rec.function_id) +
                                                " is unsupported");
                }
            default:
                throw TUnsupportedError("program_type " + to_string((int)header.rec.program_type) + " is unsupported");
        }
    };

    TFrame frame{0};

    SendIAmHereTime = now();

    SmartWeb::TCanHeader header{0};

    //   can0  0015AC0B   [8]  00 00 00 00 00 00 00 00  (CONTROLLER: JOURNAL (Get controller journal notes))
    //   can0  000AAC0B   [0]                           (CONTROLLER: GET_CHANNEL_NUMBER (Узнать количество
    //   входов/выходов)) can0  0003AC0B   [0]                           (CONTROLLER: GET_ACTIVE_PROGRAMS_LIST (Узнать
    //   список активных программ)) can0  0001AC16   [4]  0B 01 00 00              (REMOTE_CONTROL: GET_PARAMETER_VALUE)
    //   can0  0001AC16   [4]  0B 1E 00 00              (REMOTE_CONTROL: GET_PARAMETER_VALUE)
    //   can0  0001AC16   [4]  0B 02 00 00              (REMOTE_CONTROL: GET_PARAMETER_VALUE)
    //   can0  0008AC0B   [0]                           (CONTROLLER: GET_CONTROLLER_TYPE (Узнать тип контроллера))
    //   can0  0018AC0B   [1]  00                       (CONTROLLER: GET_RELAY_MAPPING (Get controller output binding))
    //   can0  0001AC16   [4]  0B 1C 00 00              (REMOTE_CONTROL: GET_PARAMETER_VALUE)
    //   can0  0001AC16   [4]  0B 1D 00 00              (REMOTE_CONTROL: GET_PARAMETER_VALUE)
    //   can0  0001AC16   [6]  0B 05 00 09 8A 2F        (REMOTE_CONTROL: GET_PARAMETER_VALUE)

    while (Enabled.load()) {
        memset(&frame, 0, sizeof(TFrame));

        bool frame_for_me = SelectTimeout(frame);

        if (!frame_for_me) {
            if (Status != DS_IDLE) {
                if (ResetConnectionTime <= now()) {
                    Status = DS_IDLE;
                    InfoMqttToSw.Log() << "[" << (int)DriverState.ProgramId << "] CONNECTION LOST: TIMEOUT. IDLE";
                }
            }
        }

        if (frame_for_me) {
            print_frame(DebugMqttToSw, frame, "[" + std::to_string(DriverState.ProgramId) + "] got frame");
        }

        try {
            switch (Status) {
                case DS_IDLE:
                    if (frame_for_me) {
                        Status = DS_RUNNING;
                        InfoMqttToSw.Log() << "[" << (int)DriverState.ProgramId << "] CONNECTION ESTABILISHED. RUNNING";
                    } else {
                        send_scheduled_i_am_here();
                        break;
                    }

                case DS_RUNNING:
                    if (frame_for_me) {
                        postpone_connection_reset();
                        header.raw = frame.can_id;
                        handle_request(header, frame.data);
                    }

                    send_scheduled_outputs();
                    send_scheduled_i_am_here();

                    break;
            }

        } catch (const TUnsupportedError& e) {
            DebugMqttToSw.Log() << "[" << (int)DriverState.ProgramId << "] " << e.what();
        } catch (const TDriverError& e) {
            WarnMqttToSw.Log() << "[" << (int)DriverState.ProgramId << "] " << e.what();
        } catch (const std::exception& e) {
            ErrorMqttToSw.Log() << "[" << (int)DriverState.ProgramId << "] " << e.what();
            exit(1);
        }
    }
    Driver->RemoveEventHandler(onHandler);
}