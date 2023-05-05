#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <unordered_map>

#include <wblib/log.h>
#include <wblib/wbmqtt.h>

#include "CanPort.h"
#include "smart_web_conventions.h"

using TTimePoint = std::chrono::time_point<std::chrono::steady_clock>;
using TTimeIntervalMs = std::chrono::milliseconds;
using TTimeIntervalS = std::chrono::seconds;
using TTimeIntervalMin = std::chrono::minutes;

const uint8_t CONTROLLER_OUTPUT_MAX = 32;

enum EDriverStatus
{
    DS_IDLE,
    DS_RUNNING
};

struct TMqttChannel
{
    std::string device;
    std::string control;

    void from_string(const std::string& deviceControl);
    bool is_initialized() const;
    std::string to_string() const;

    static std::string to_string(const std::string& device, const std::string& control);
};

struct TChannelState
{
    TTimePoint SendTimePoint;    // next send timepoint
    TTimePoint SendEndTimePoint; // when to stop sending messages

    void postpone_send();
    void postpone_send_end();
    void schedule_to_send();
};

struct TBroadcastChannel: TMqttChannel, TChannelState
{
    SmartWeb::TMappingPoint mapping_point;

    void schedule_to_send(const SmartWeb::TMappingPoint& mp);
};

struct TMqttChannelTiming
{
private:
    TTimePoint LastUpdateTimePoint;
    mutable std::mutex LastUpdateTimePointMutex;

public:
    TTimeIntervalMin ValueTimeoutMin = TTimeIntervalMin(-1);

    TMqttChannelTiming() = default;
    TMqttChannelTiming(const TMqttChannelTiming& other);

    void refresh_last_update_timepoint();
    bool is_timed_out() const;
};

struct TMqttToSmartWebConfig
{
    uint8_t ProgramId;
    std::unordered_map<uint32_t, TMqttChannel> ParameterMapping;
    std::unordered_map<std::string, TMqttChannelTiming> MqttChannelsTiming;
    TBroadcastChannel OutputMapping[CONTROLLER_OUTPUT_MAX];
    uint8_t ParameterCount = 0;
};

class TMqttToSmartWebGateway: public CAN::IFrameHandler
{
    TMqttToSmartWebConfig DriverState;
    uint8_t CONTROLLER_TYPE; // SWX for now
    std::shared_ptr<CAN::IPort> CanPort;
    WBMQTT::PDeviceDriver Driver;

    EDriverStatus Status = DS_IDLE;
    TTimePoint SendIAmHereTime;
    TTimePoint ResetConnectionTime;

    std::mutex CanFramesMutex;
    std::condition_variable CanFramesCv;
    std::queue<CAN::TFrame> CanFrames;

    std::thread Thread;
    std::atomic_bool Enabled;

    static bool FilterIsSet;
    static std::mutex StartupMutex;

    bool SelectTimeout(CAN::TFrame& frame);
    void TaskFn();
    bool Handle(const CAN::TFrame& frame);
    bool IsForMe(const SmartWeb::TCanHeader& header, const CAN::TFrameData& data) const;

public:
    TMqttToSmartWebGateway(const TMqttToSmartWebConfig& config,
                           std::shared_ptr<CAN::IPort> canPort,
                           WBMQTT::PDeviceDriver driver);
    ~TMqttToSmartWebGateway();

    void Stop();
};

void print_frame(WBMQTT::TLogger& logger, const CAN::TFrame& frame, const std::string& prefix);