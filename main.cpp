#include "smart_web_conventions.h"
#include "exceptions.h"

#include <wbmqtt/wbmqtt.h>

#if defined(__APPLE__) || defined(__APPLE_CC__)
#   include <json/json.h>
#else
#   include <jsoncpp/json/json.h>
#endif

#include <iostream>
#include <fstream>
#include <cstdio>
#include <getopt.h>
#include <unistd.h>
#include <chrono>

#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <linux/can.h>
#include <linux/can/raw.h>

using namespace WBMQTT;
using namespace std;

using TTimePoint        = chrono::time_point<chrono::steady_clock>;
using TTimeIntervalMs   = chrono::milliseconds;
using TTimeIntervalS    = chrono::seconds;
using TTimeIntervalMin  = chrono::minutes;

using TFrame        = struct can_frame;
using TFrameData    = decltype(TFrame::data);

const auto             DRIVER_NAME            = "wb-mqtt-can";
const auto             LIBWBMQTT_DB_FILE      = "/tmp/libwbmqtt.db";

const uint8_t          CONTROLLER_TYPE        = 0x3;    // SWX for now

const TTimeIntervalMs  SEND_TIME_INTERVAL_MS  = TTimeIntervalMs(1000);
const TTimeIntervalS   KEEP_ALIVE_INTERVAL_MS = TTimeIntervalS(10);
const TTimeIntervalMin SEND_TIMEOUT_MIN       = TTimeIntervalMin(10);



struct TChannelState
{
    TTimePoint Timeout;
    TTimePoint Send;
};

enum EDriverStatus
{
    DS_INIT, DS_HANDSHAKE, DS_IDLE
};

struct TMqttChannel
{
    string device, channel;
};

struct
{
    EDriverStatus                           Status = DS_INIT;
    TTimePoint                              SendIAmHere;
    unordered_map<string, TMqttChannel>     Mapping;
} DriverState;

void print_usage_and_exit()
{
    printf("Usage:\n wb-mqtt-smartweb [options]\n");
    printf("Options:\n");
    printf("\t-d          \t\t\t enable debuging output\n");
    printf("\t-c config   \t\t\t config file\n");
    printf("\t-p PORT     \t\t\t set to what port wb-mqtt-serial should connect (default: 1883)\n");
    printf("\t-H IP       \t\t\t set to what IP wb-mqtt-serial should connect (default: localhost)\n");
    printf("\t-i interface       \t\t\t CAN interface (default: can0)\n");
    exit(-1);
}

int connect_can(const string & ifname)
{
    int s;
    struct sockaddr_can addr;
    struct ifreq ifr;

    if ((s = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0)
    {
        perror("Error while opening socket");
        exit(1);
    }

    strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
	ifr.ifr_name[IFNAMSIZ - 1] = '\0';
	ifr.ifr_ifindex = if_nametoindex(ifr.ifr_name);
	if (!ifr.ifr_ifindex) {
		perror("if_nametoindex");
		return 1;
	}

    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    Info.Log() << ifname.c_str() << " at index " << ifr.ifr_ifindex;

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("Error in socket bind");
        exit(2);
    }

    return s;
}

TTimePoint now()
{
    return chrono::steady_clock::now();
}

void print_frame(const TFrame & frame)
{
    SmartWeb::TCanHeader header;
    header.raw = frame.can_id;

    if (header.rec.program_type != SmartWeb::PT_REMOTE_CONTROL || header.rec.function_id != SmartWeb::RemoteControl::Function::GET_PARAMETER_VALUE) {
        return;
    }

    Debug.Log() << "frame can_id: " << hex << frame.can_id;

    Debug.Log() << "program_type: " << dec << (int)header.rec.program_type;
    Debug.Log() << "program_id: " << dec << (int)header.rec.program_id;
    Debug.Log() << "function_id: " << dec << (int)header.rec.function_id;
    Debug.Log() << "message_format: " << dec << (int)header.rec.message_format;
    Debug.Log() << "message_type: " << dec << (int)header.rec.message_type;

    Debug.Log() << "frame data len: " << dec << (int)frame.can_dlc;

    Debug.Log() << "frame data: ";
    if (Debug.IsEnabled()) {
        for (int i = 0; i < frame.can_dlc; ++i)
        {
            printf("%02x", (int)frame.data[i]);
        }
        cout << endl;
    }
}

int main(int argc, char *argv[])
{
    string config_fname;
    string ifname = "can0";
    uint8_t program_id = 0;

    TMosquittoMqttConfig mqttConfig;
    bool debug = false;

    int c;

    while ( (c = getopt(argc, argv, "c:h:p:di:")) != -1) {
        //~ int this_option_optind = optind ? optind : 1;
        switch (c) {
        case 'd':
            debug = true;
            break;
        case 'c':
            config_fname = optarg;
            break;
        case 'p':
            mqttConfig.Port = stoi(optarg);
            break;
        case 'h':
            mqttConfig.Host = optarg;
            break;
        case 'i':
            ifname = optarg;
            break;
        case '?':
        default:
            print_usage_and_exit();
        }
    }

    if (config_fname.empty()) {
        print_usage_and_exit();
    }

    {
        Json::Value config;
        {
            fstream config_file (config_fname);
            if (config_file.fail()) {
                throw runtime_error("Serial driver configuration file not found: " + config_fname);
            }

            Json::Reader reader;
            if(!reader.parse(config_fname, config, false)) {
                throw runtime_error("Failed to parse JSON: " + reader.getFormatedErrorMessages());
            }
        }

        #define REQUIRE(v) if (!config.isMember("v")) {                    \
            throw runtime_error("Malformed JSON config: v is required");   \
        }

        REQUIRE(program_id)
        REQUIRE(channels)

        #undef REQUIRE

        program_id = config["program_id"].asUInt();
        const auto & channels = config["channels"];

        if (channels.size() == 0) {
            throw runtime_error("Malformed JSON config: no channels");
        }

        for (Json::ArrayIndex i = 0; i < channels.size(); ++i) {
            auto channel = channels[i];

            #define REQUIRE(v) if (!channel.isMember("v")) {                                    \
                throw runtime_error("Malformed JSON config: no \"v\" in channel " + to_string(i));  \
            }

            REQUIRE(device);
            REQUIRE(channel);
            REQUIRE(parameter_id);

            #undef REQUIRE


            DriverState.Mapping[channel["parameter_id"].asString()] = TMqttChannel{
                channel["device"].asString(),
                channel["channel"].asString()
            };
        }
    }

    Debug.SetEnabled(debug);
    Info.SetEnabled(debug);

    auto mqtt = NewMosquittoMqttClient(mqttConfig);
    auto mqtt_driver = NewDriver(TDriverArgs{}
        .SetId(DRIVER_NAME)
        .SetBackend(NewDriverBackend(mqtt))
        .SetUseStorage(true)
        .SetReownUnknownDevices(true)
        .SetStoragePath(LIBWBMQTT_DB_FILE)
    );

    mqtt_driver->StartLoop();
    mqtt_driver->WaitForReady();

    auto fd = connect_can(ifname);

    auto get_response_frame = [&](TFrame frame) {
        SmartWeb::TCanHeader header;
        header.raw = frame.can_id;

        if (header.rec.message_type != SmartWeb::MT_MSG_REQUEST) {
            throw TFrameError("Frame error: response to NOT request frame");
        }

        if (header.rec.program_id != program_id) {
            throw TFrameError("Frame error: response to request frame for different device (" + to_string((int)header.rec.program_id) + ")");
        }

        header.rec.message_type = SmartWeb::MT_MSG_RESPONSE;

        frame.can_id = header.raw;
        return frame;
    };

    auto postpone_i_am_here = [&]{ DriverState.SendIAmHere = now() + KEEP_ALIVE_INTERVAL_MS; };

    auto i_am_here = [&](){
        postpone_i_am_here();

        Debug.Log() << "Send I_AM_HERE";

        SmartWeb::TCanHeader header;

        header.rec.program_type = SmartWeb::PT_CONTROLLER;
        header.rec.program_id   = program_id;
        header.rec.function_id  = SmartWeb::Controller::Function::I_AM_HERE;
        header.rec.message_format = SmartWeb::MF_FORMAT_0;
        header.rec.message_type = SmartWeb::MT_MSG_RESPONSE;

        TFrame frame {0};
        frame.can_id = header.raw | CAN_EFF_FLAG;
        frame.can_dlc = 1;
        frame.data[0] = CONTROLLER_TYPE;

        return frame;
    };

    auto get_channel_number = [&](TFrame & response){
        Debug.Log() << "Send channel number";

        auto channel_number = DriverState.Mapping.size();
        response.can_dlc = 2;
        response.data[0] = 0xFF & channel_number;
        response.data[1] = 0xFF & channel_number >> 8;
    };

    auto get_parameter_value = [&](TFrame & response, const TFrameData & data){
        union {
            uint32_t raw;
            struct {
                uint8_t  program_type,
                         parameter_id;
                uint16_t parameter_index;
            };
        } request_data;

        memcpy(&request_data.raw, data, sizeof request_data);

        Debug.Log() << "Get parameter";
        Debug.Log() << "\t program_type " << (int)request_data.program_type;
        Debug.Log() << "\t parameter_id " << (int)request_data.parameter_id;
        Debug.Log() << "\t parameter_index " << (int)request_data.parameter_index;

        if (request_data.program_type != SmartWeb::PT_CONTROLLER) {
            throw TUnsupportedError("Unsupported program type " + to_string((int)request_data.program_type) + " for GET_PARAMETER_VALUE");
        }

        const auto & itDeviceChannel = DriverState.Mapping.find(to_string(request_data.parameter_index));

        if (itDeviceChannel == DriverState.Mapping.end()) {
            throw TUnsupportedError("Unsupported parameter index " + to_string((int)request_data.parameter_index));
        }

        auto parameter_value = mqtt_driver->BeginTx()->GetDevice(itDeviceChannel->second.device)->GetControl(itDeviceChannel->second.channel)->GetValue().As<double>();

        uint16_t value = static_cast<uint16_t>(parameter_value);

        response.can_dlc = 6;

        memcpy(response.data, data, 4);
        response.data[4] = 0xFF & value >> 8;
        response.data[5] = 0xFF & value;
    };

    auto get_controller_type = [&](TFrame & response){
        Debug.Log() << "Send controller type";

        response.can_dlc = 1;
        response.data[0] = CONTROLLER_TYPE;
    };

    auto handshake_handle = [&](TFrame & response, const SmartWeb::TCanHeader & header, const TFrameData & data) {
        switch (header.rec.program_type) {
            case SmartWeb::PT_CONTROLLER:
                switch (header.rec.function_id) {
                    case SmartWeb::Controller::Function::GET_CHANNEL_NUMBER:
                        return get_channel_number(response);
                    case SmartWeb::Controller::Function::GET_CONTROLLER_TYPE:
                        return get_controller_type(response);
                    default:
                        throw TUnsupportedError("function id " + to_string((int)header.rec.function_id) + " is unsupported");
                }
            case SmartWeb::PT_REMOTE_CONTROL:
                switch (header.rec.function_id) {
                    case SmartWeb::RemoteControl::Function::GET_PARAMETER_VALUE:
                        return get_parameter_value(response, data);
                    default:
                        throw TUnsupportedError("function id " + to_string((int)header.rec.function_id) + " is unsupported");
                }
            default:
                throw TUnsupportedError("program_type " + to_string((int)header.rec.program_type) + " is unsupported");
        }
    };

    int nbytes;
    TFrame frame;

    DriverState.SendIAmHere = now();

    SmartWeb::TCanHeader header;

//   can0  0015AC0B   [8]  00 00 00 00 00 00 00 00
//   can0  000AAC0B   [0]
//   can0  0003AC0B   [0]
//   can0  0001AC16   [4]  0B 01 00 00
//   can0  0001AC16   [4]  0B 1E 00 00
//   can0  0001AC16   [4]  0B 02 00 00
//   can0  0008AC0B   [0]
//   can0  0018AC0B   [1]  00
//   can0  0001AC16   [4]  0B 1C 00 00
//   can0  0001AC16   [4]  0B 1D 00 00
//   can0  0001AC16   [6]  0B 05 00 09 8A 2F


    while (true)
    {
        memset(&frame, 0, sizeof(TFrame));

        nbytes = read(fd, &frame, sizeof(TFrame));

        header.raw = frame.can_id;

        if (header.rec.program_id == program_id) {
            Debug.Log() << "--------read " << nbytes << " bytes----------";
            print_frame(frame);
        }

        try {
            TFrame response_frame {0};
            switch (DriverState.Status) {
                case DS_INIT:
                    if (header.rec.program_id == program_id) {
                        DriverState.Status = DS_HANDSHAKE;
                        Debug.Log() << "begin handshake";
                    } else {
                        if (DriverState.SendIAmHere <= now()) {
                            response_frame = i_am_here();
                            break;
                        }
                        continue;
                    }

                case DS_HANDSHAKE:
                    if (header.rec.program_id != program_id) {

                    }
                    try {
                        response_frame = get_response_frame(frame);
                    } catch (const TFrameError &) {
                        continue;
                    }

                    handshake_handle(response_frame, header, frame.data);
                    break;

                case DS_IDLE:
                    if (header.rec.program_id == SmartWeb::PT_CONTROLLER &&
                        header.rec.message_type == SmartWeb::MT_MSG_REQUEST &&
                        header.rec.function_id == SmartWeb::Controller::Function::I_AM_HERE)
                    {
                        response_frame = i_am_here();
                        break;
                    }

                    continue;
            }

            // response_frame.can_id |= CAN_EFF_FLAG;
            nbytes = write(fd, &response_frame, CAN_MTU);

            if (nbytes < CAN_MTU) {
                Error.Log() << "write error: " << strerror(errno);
            } else {
                Debug.Log() << "--------write " << nbytes << " bytes--------";
            }
            print_frame(response_frame);

        } catch (const TDriverError & e) {
            Warn.Log() << e.what();
        }
    }
}
