#pragma once

#include <map>
#include <tgmath.h>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <wblib/wbmqtt.h>

#include "CanPort.h"
#include "scheduler.h"
#include "smart_web_conventions.h"

const auto DEFAULT_POLL_INTERVAL_MS = std::chrono::milliseconds(500);

/**
 * @brief Interface for classes performing conversion from data received
 *        from CAN in SmartWeb encoding to string for publishing in MQTT and vice verca.
 */
class ISmartWebCodec
{
public:
    virtual ~ISmartWebCodec() = default;

    /**
     * @brief Decodes data from byte array received in CAN frame to string
     *        Can throw exception if conversion is impossible.
     *
     * @param buf array received from CAN
     */
    virtual std::string Decode(const uint8_t* buf) const = 0;

    /**
     * @brief Encodes data from string to byte array for sending over SmartWeb CAN.
     *        Can throw exception if conversion is impossible.
     *
     * @param buf array received from CAN
     */
    virtual std::vector<uint8_t> Encode(const std::string& value) const = 0;

    /**
     * @brief Returns human readable name of the class
     */
    virtual std::string GetName() const = 0;
};

/**
 * @brief Class performs conversion to integer value with division by Div
 *
 * @tparam Int integer type
 * @tparam Div value to divide conversion result to
 */
template<class Int, uint32_t Div> class TIntCodec: public ISmartWebCodec
{
public:
    std::string Decode(const uint8_t* buf) const override
    {
        Int res;
        memcpy(&res, buf, sizeof(Int));
        return WBMQTT::FormatFloat(res / double(Div));
    }

    std::vector<uint8_t> Encode(const std::string& value) const override
    {
        Int v = std::stod(value.c_str()) * Div;
        std::vector<uint8_t> res;
        for (size_t i = 0; i < sizeof(Int); ++i) {
            res.push_back(v & 0xFF);
            v >>= 8;
        }
        return res;
    }

    std::string GetName() const override
    {
        return std::string("TIntCodec<") + (std::is_signed<Int>::value ? "signed " : "unsigned ") +
               std::to_string(sizeof(Int)) + ", " + std::to_string(Div) + ">";
    }
};

/**
 * @brief Class performs conversion to integer value
 *
 * @tparam Int integer type
 * @tparam Div value to divide conversion result to
 */
template<class Int> class TIntCodec<Int, 1>: public ISmartWebCodec
{
public:
    std::string Decode(const uint8_t* buf) const override
    {
        Int res;
        memcpy(&res, buf, sizeof(Int));
        return std::to_string(res);
    }

    std::vector<uint8_t> Encode(const std::string& value) const override
    {
        Int v = std::stod(value.c_str());
        std::vector<uint8_t> res;
        for (size_t i = 0; i < sizeof(Int); ++i) {
            res.push_back(v & 0xFF);
            v >>= 8;
        }
        return res;
    }

    std::string GetName() const override
    {
        return std::string("TIntCodec<") + (std::is_signed<Int>::value ? "signed " : "unsigned ") +
               std::to_string(sizeof(Int)) + ">";
    }
};

/**
 * @brief Class performs conversion from byte to one of string values specified on initialization
 */
class TEnumCodec: public ISmartWebCodec
{
    std::map<uint8_t, std::string> Values;
    std::unordered_map<std::string, uint8_t> Keys;

public:
    TEnumCodec(const std::map<uint8_t, std::string>& values);
    std::string Decode(const uint8_t* buf) const override;
    std::vector<uint8_t> Encode(const std::string& value) const override;
    std::string GetName() const override;
};

/**
 * @brief Class performs conversion from 2 byte integer with division by 10.
 */
class TSensorCodec: public ISmartWebCodec
{
public:
    /**
     * @brief Decodes and checks sensor error states and throws during decoding on errors.
     */
    std::string Decode(const uint8_t* buf) const override;
    std::vector<uint8_t> Encode(const std::string& value) const override;
    std::string GetName() const override;
};

/**
 * @brief Class performs conversion from 2 byte integer to switch state (0/1).
 */
class TOnOffSensorCodec: public ISmartWebCodec
{
public:
    /**
     * @brief Decodes and checks sensor error states and throws during decoding on errors.
     */
    std::string Decode(const uint8_t* buf) const override;
    std::vector<uint8_t> Encode(const std::string& value) const override;
    std::string GetName() const override;
};

/**
 * @brief Class performs conversion from byte integer to PWM percent.
 *        Values 254 or 255 are equal to 100%.
 */
class TPwmCodec: public ISmartWebCodec
{
public:
    std::string Decode(const uint8_t* buf) const override;
    std::vector<uint8_t> Encode(const std::string& value) const override;
    std::string GetName() const override;
};

/**
 * @brief Class performs conversion from byte to switch state (0/1).
 */
class TOutputCodec: public ISmartWebCodec
{
public:
    std::string Decode(const uint8_t* buf) const override;
    std::vector<uint8_t> Encode(const std::string& value) const override;
    std::string GetName() const override;
};

struct TSmartWebClass;

struct TSmartWebParameter
{
    uint32_t Id;
    std::string Name;
    std::string Type;
    bool ReadOnly = true;
    std::unique_ptr<ISmartWebCodec> Codec;
    const TSmartWebClass* ProgramClass;
    uint32_t Order;
};

enum class TDeviceClassSource
{
    BUILTIN,
    USER
};

struct TSmartWebClass
{
    uint8_t Type;
    std::string Name;
    std::vector<std::string> ParentClasses;

    TDeviceClassSource Source;

    //! id to TSmartWebParameter mapping
    std::map<uint32_t, std::shared_ptr<TSmartWebParameter>> Inputs;

    //! id to TSmartWebParameter mapping
    std::map<uint32_t, std::shared_ptr<TSmartWebParameter>> Outputs;

    //! id to TSmartWebParameter mapping
    std::map<uint32_t, std::shared_ptr<TSmartWebParameter>> Parameters;
};

struct TSmartWebParameterControl
{
    uint8_t ProgramId;
    const TSmartWebParameter* Parameter;
};

struct TSmartWebToMqttConfig
{
    std::chrono::milliseconds PollInterval = DEFAULT_POLL_INTERVAL_MS;

    //! Program type to TSmartWebClass mapping
    std::unordered_map<uint8_t, std::shared_ptr<TSmartWebClass>> Classes;
};

void AddRequests(std::vector<CAN::TFrame>& requests,
                 const std::string& name,
                 const TSmartWebClass& cl,
                 uint8_t programId,
                 const std::unordered_map<uint8_t, std::shared_ptr<TSmartWebClass>>& classes);

CAN::TFrame MakeSetParameterValueRequest(const TSmartWebParameterControl& param, const std::string& value);

class TSmartWebToMqttGateway: public CAN::IFrameHandler
{
    std::shared_ptr<CAN::IPort> CanPort;
    TSmartWebToMqttConfig Config;
    WBMQTT::PDeviceDriver Driver;
    WBMQTT::PDriverEventHandlerHandle EventHandler;
    std::vector<std::string> DeviceIds;

    std::mutex RequestMutex;
    std::vector<CAN::TFrame> Requests;
    size_t RequestIndex;
    std::unique_ptr<IScheduler> Scheduler;

    std::mutex KnownProgramsMutex;

    //! Program id to TSmartWebClass mapping
    std::unordered_map<uint8_t, TSmartWebClass*> KnownPrograms;

    void HandleMapping();
    void AddProgram(const CAN::TFrame& frame);
    void HandleGetValueResponse(const CAN::TFrame& frame);

    void SetParameter(const std::map<uint32_t, std::shared_ptr<TSmartWebParameter>>& params,
                      uint8_t parameterId,
                      const uint8_t* data,
                      uint8_t programId);

    WBMQTT::TControlArgs MakeControlArgs(uint8_t programId,
                                         const TSmartWebParameter& param,
                                         const std::string& value,
                                         bool error);
    bool Handle(const CAN::TFrame& frame);

public:
    TSmartWebToMqttGateway(const TSmartWebToMqttConfig& config,
                           std::shared_ptr<CAN::IPort> canPort,
                           WBMQTT::PDeviceDriver driver);

    ~TSmartWebToMqttGateway();
};