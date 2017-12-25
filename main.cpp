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

const TTimeIntervalS   KEEP_ALIVE_INTERVAL_S = TTimeIntervalS(10);      // if nothing else to do - each 10 seconds send I_AM_HERE
const TTimeIntervalMin CONNECTION_TIMEOUT_MIN = TTimeIntervalMin(10);   // after 10 minutes without any messages connection is considered lost
const TTimeIntervalMs  READ_TIMEOUT_MS        = TTimeIntervalMs(1000);  // 1 sec for messages waiting


enum EDriverStatus
{
    DS_IDLE, DS_RUNNING
};

struct TMqttChannel
{
    string device, channel;
};

struct
{
    EDriverStatus                           Status = DS_IDLE;
    TTimePoint                              SendIAmHereTime;
    TTimePoint                              ResetConnectionTime;
    unordered_map<uint32_t, TMqttChannel>   Mapping;
} DriverState;

TLogger::TOutput logger_stdout {std::cout};

TLogger LocalDebug("DEBUG: ", logger_stdout, 15);

void print_usage_and_exit()
{
    printf("Usage:\n wb-mqtt-smartweb [options]\n");
    printf("Options:\n");
    printf("\t-d level    \t\t\t enable debuging output\n");
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

bool select_timeout(int fd)
{
    fd_set rfds;
    struct timeval tv, *tvp = 0;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    tv.tv_sec = READ_TIMEOUT_MS.count() / 1000;
    tv.tv_usec = READ_TIMEOUT_MS.count() * 1000;
    tvp = &tv;

    int r = select(fd + 1, &rfds, NULL, NULL, tvp);
    if (r < 0)
        throw TDriverError("select() failed");

    return r > 0;
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

    LocalDebug.Log() << "frame can_id: " << hex << frame.can_id;

    LocalDebug.Log() << "program_type: " << dec << (int)header.rec.program_type;
    LocalDebug.Log() << "program_id: " << dec << (int)header.rec.program_id;
    LocalDebug.Log() << "function_id: " << dec << (int)header.rec.function_id;
    LocalDebug.Log() << "message_format: " << dec << (int)header.rec.message_format;
    LocalDebug.Log() << "message_type: " << dec << (int)header.rec.message_type;

    LocalDebug.Log() << "frame data len: " << dec << (int)frame.can_dlc;

    LocalDebug.Log() << "frame data: ";
    if (LocalDebug.IsEnabled()) {
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
    uint8_t debug = 0;

    int c;

    while ( (c = getopt(argc, argv, "c:h:p:d:i:")) != -1) {
        //~ int this_option_optind = optind ? optind : 1;
        switch (c) {
        case 'd':
            debug = stoul(optarg);
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
            if(!reader.parse(config_file, config, false)) {
                throw runtime_error("Failed to parse JSON: " + reader.getFormatedErrorMessages());
            }
        }

        #define REQUIRE(v) if (!config.isMember(#v)) {                    \
            throw runtime_error("Malformed JSON config: " #v " is required");   \
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

            #define REQUIRE(o, v) [&]{if (!o.isMember(#v)) {                                    \
                throw runtime_error("Malformed JSON config: no \"" #v "\" in " #o + to_string(i));  \
            } \
            return o[#v];}()

            const auto & mqtt = REQUIRE(channel, mqtt);
            const auto & can = REQUIRE(channel, can);

            const auto & mqtt_device = REQUIRE(mqtt, device).asString();
            const auto & mqtt_channel = REQUIRE(mqtt, channel).asString();

            SmartWeb::TParameterInfo parameter_info {0};
            {
                parameter_info.parameter_id = REQUIRE(can, parameter_id).asUInt();
                parameter_info.program_type = REQUIRE(can, program_type).asUInt();
                parameter_info.index = REQUIRE(can, parameter_index).asUInt();
            }

            #undef REQUIRE

            Info.Log() << "Map parameter";
            Info.Log() << "\t program_type " << (int)parameter_info.program_type;
            Info.Log() << "\t parameter_id " << (int)parameter_info.parameter_id;
            Info.Log() << "\t parameter_index " << (int)parameter_info.index;
            Info.Log() << "\t raw " << (int)parameter_info.raw;

            DriverState.Mapping[parameter_info.raw] = { mqtt_device, mqtt_channel };
        }
    }

    Debug.SetEnabled(false);
    LocalDebug.SetEnabled(false);
    Info.SetEnabled(false);

    switch (debug) {
        case 3:
            Debug.SetEnabled(true);
        case 2:
            LocalDebug.SetEnabled(true);
        case 1:
            Info.SetEnabled(true);
        default:
            break;
    }

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
    mqtt_driver->SetFilter(GetAllDevicesFilter());

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

    auto postpone_i_am_here = [&]{ DriverState.SendIAmHereTime = now() + KEEP_ALIVE_INTERVAL_S; };
    auto postpone_connection_reset = [&]{ DriverState.ResetConnectionTime = now() + CONNECTION_TIMEOUT_MIN; };

    auto i_am_here = [&](){
        postpone_i_am_here();

        LocalDebug.Log() << "Send I_AM_HERE";

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
        LocalDebug.Log() << "Send channel number";

        auto channel_number = DriverState.Mapping.size();
        response.can_dlc = 2;
        response.data[0] = 0xFF & channel_number;
        response.data[1] = 0xFF & channel_number >> 8;
    };

    auto get_parameter_value = [&](TFrame & response, const TFrameData & data){
        SmartWeb::TParameterData parameter_data {0};

        memcpy(&parameter_data.raw, data, 4);

        LocalDebug.Log() << "Get parameter";
        LocalDebug.Log() << "\t program_type " << (int)parameter_data.program_type;
        LocalDebug.Log() << "\t parameter_id " << (int)parameter_data.parameter_id;
        LocalDebug.Log() << "\t parameter_index " << (int)parameter_data.indexed_parameter.index;
        LocalDebug.Log() << "\t raw " << (int)parameter_data.raw_info;

        if (parameter_data.program_type != SmartWeb::PT_CONTROLLER) {
            throw TUnsupportedError("Unsupported program type " + to_string((int)parameter_data.program_type) + " for GET_PARAMETER_VALUE");
        }

        const auto & itDeviceChannel = DriverState.Mapping.find(parameter_data.raw_info);

        int16_t value = SmartWeb::SENSOR_UNDEFINED;

        if (itDeviceChannel == DriverState.Mapping.end()) {
            Warn.Log() << "Unmapped parameter: "
                "\n\t program_type: "  << (int)parameter_data.program_type <<
                "\n\t parameter_id: " << (int)parameter_data.parameter_id <<
                "\n\t parameter_index: " << (int)parameter_data.indexed_parameter.index;
        } else {
            value = SmartWeb::SensorData::FromDouble(
                mqtt_driver->BeginTx()->GetDevice(itDeviceChannel->second.device)->GetControl(itDeviceChannel->second.channel)->GetValue().As<double>()
            );
        }

        response.can_dlc = 5;

        memset(response.data, 0, sizeof response.data);

        memcpy(parameter_data.indexed_parameter.value, &value, sizeof value);

        memcpy(response.data, &parameter_data.raw, 5);

        Info.Log() << " Parameter {type: " << (int)parameter_data.program_type
                             << ", id: " << (int)parameter_data.parameter_id
                             << ", index: " << (int)parameter_data.indexed_parameter.index
                             << "} <== " << SmartWeb::SensorData::ToDouble(value);
    };

    auto get_controller_type = [&](TFrame & response){
        LocalDebug.Log() << "Send controller type";

        response.can_dlc = 1;
        response.data[0] = CONTROLLER_TYPE;
    };

    auto handle_request = [&](TFrame & response, const SmartWeb::TCanHeader & header, const TFrameData & data) {
        switch (header.rec.program_type) {
            case SmartWeb::PT_CONTROLLER:
                switch (header.rec.function_id) {
                    case SmartWeb::Controller::Function::GET_CHANNEL_NUMBER:
                        return get_channel_number(response);
                    case SmartWeb::Controller::Function::GET_CONTROLLER_TYPE:
                        return get_controller_type(response);
                    case SmartWeb::Controller::Function::I_AM_HERE:
                        response = i_am_here();
                        return;
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
    TFrame frame {0};

    DriverState.SendIAmHereTime = now();

    SmartWeb::TCanHeader header {0};

//   can0  0015AC0B   [8]  00 00 00 00 00 00 00 00  (CONTROLLER: JOURNAL (Get controller journal notes))
//   can0  000AAC0B   [0]                           (CONTROLLER: GET_CHANNEL_NUMBER (Узнать количество входов/выходов))
//   can0  0003AC0B   [0]                           (CONTROLLER: GET_ACTIVE_PROGRAMS_LIST (Узнать список активных программ))
//   can0  0001AC16   [4]  0B 01 00 00              (REMOTE_CONTROL: GET_PARAMETER_VALUE)
//   can0  0001AC16   [4]  0B 1E 00 00              (REMOTE_CONTROL: GET_PARAMETER_VALUE)
//   can0  0001AC16   [4]  0B 02 00 00              (REMOTE_CONTROL: GET_PARAMETER_VALUE)
//   can0  0008AC0B   [0]                           (CONTROLLER: GET_CONTROLLER_TYPE (Узнать тип контроллера))
//   can0  0018AC0B   [1]  00                       (CONTROLLER: GET_RELAY_MAPPING (Get controller output binding))
//   can0  0001AC16   [4]  0B 1C 00 00              (REMOTE_CONTROL: GET_PARAMETER_VALUE)
//   can0  0001AC16   [4]  0B 1D 00 00              (REMOTE_CONTROL: GET_PARAMETER_VALUE)
//   can0  0001AC16   [6]  0B 05 00 09 8A 2F        (REMOTE_CONTROL: GET_PARAMETER_VALUE)


    while (true)
    {
        memset(&frame, 0, sizeof(TFrame));

        if (select_timeout(fd)) {
            nbytes = read(fd, &frame, sizeof(TFrame));
        } else {
            if (DriverState.Status != DS_IDLE) {
                if (DriverState.ResetConnectionTime <= now()) {
                    DriverState.Status = DS_IDLE;
                    Info.Log() << "CONNECTION LOST: TIMEOUT. IDLE";
                }
            }
        }

        header.raw = frame.can_id;

        if (header.rec.program_id == program_id) {
            LocalDebug.Log() << "--------read " << nbytes << " bytes----------";
            print_frame(frame);
        }

        try {
            TFrame response_frame {0};
            switch (DriverState.Status) {
                case DS_IDLE:
                    if (header.rec.program_id == program_id) {
                        DriverState.Status = DS_RUNNING;
                        Info.Log() << "CONNECTION ESTABILISHED. RUNNING";
                    } else {
                        if (DriverState.SendIAmHereTime <= now()) {
                            response_frame = i_am_here();
                            break;
                        }
                        continue;
                    }

                case DS_RUNNING:
                    if (header.rec.program_id == program_id) {
                        postpone_connection_reset();
                    } else {
                        continue;
                    }

                    try {
                        response_frame = get_response_frame(frame);
                    } catch (const TFrameError &) {
                        continue;
                    }

                    handle_request(response_frame, header, frame.data);
                    break;
            }

            // response_frame.can_id |= CAN_EFF_FLAG;
            nbytes = write(fd, &response_frame, CAN_MTU);

            if (nbytes < static_cast<int>(CAN_MTU)) {
                Error.Log() << "write error: " << strerror(errno);
            } else {
                LocalDebug.Log() << "--------write " << nbytes << " bytes--------";
            }
            print_frame(response_frame);

        } catch (const TDriverError & e) {
            Warn.Log() << e.what();
        }
    }
}
