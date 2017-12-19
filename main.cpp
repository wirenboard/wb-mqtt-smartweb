#include "smart_web_conventions.h"

#include <wbmqtt/wbmqtt.h>

#include <iostream>
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
using TTimeIntervalMin  = chrono::minutes;


const auto             DRIVER_NAME           = "wb-mqtt-can";
const auto             LIBWBMQTT_DB_FILE     = "/tmp/libwbmqtt.db";
const TTimeIntervalMs  SEND_TIME_INTERVAL_MS = TTimeIntervalMs(1000);
const TTimeIntervalMin SEND_TIMEOUT_MIN      = TTimeIntervalMin(10);

struct TChannelState
{
    TTimePoint timeout;
    TTimePoint send;

};

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

int connect_can(const string &ifname)
{
    int s;
    struct sockaddr_can addr;
    struct ifreq ifr;

    if ((s = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0)
    {
        perror("Error while opening socket");
        exit(1);
    }

    strcpy(ifr.ifr_name, ifname.c_str());
    ioctl(s, SIOCGIFINDEX, &ifr);

    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    printf("%s at index %d\n", ifname.c_str(), ifr.ifr_ifindex);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("Error in socket bind");
        exit(2);
    }

    return s;
}

int main(int argc, char *argv[])
{
    string config_fname;
    string ifname = "can0";

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

    Debug.SetEnabled(debug);
    Info.SetEnabled(debug);

    auto mqtt = NewMosquittoMqttClient(mqttConfig);
    auto driver = NewDriver(TDriverArgs{}
        .SetId(DRIVER_NAME)
        .SetBackend(NewDriverBackend(mqtt))
        .SetUseStorage(true)
        .SetReownUnknownDevices(true)
        .SetStoragePath(LIBWBMQTT_DB_FILE)
    );

    driver->StartLoop();
    driver->WaitForReady();

    auto fd = connect_can(ifname);

    int nbytes;
    struct can_frame frame;

    while (true)
    {
        memset(&frame, 0, sizeof frame);

        nbytes = read(fd, &frame, sizeof frame);

        SmartWeb::TCanHeader header;

        header.raw = frame.can_id;

        cout << "--------read " << nbytes << " bytes----------" << endl;
        cout << "frame can_id: " << hex << frame.can_id << endl;

        cout << "program_type: " << dec << (int)header.rec.program_type << endl;
        cout << "program_id: " << dec << (int)header.rec.program_id << endl;
        cout << "function_id: " << dec << (int)header.rec.function_id << endl;
        cout << "message_format: " << dec << (int)header.rec.message_format << endl;
        cout << "message_type: " << dec << (int)header.rec.message_type << endl;

        cout << "frame can_dlc: " << dec << (int)frame.can_dlc << endl;

        cout << "frame data: ";
        for (int i = 0; i < frame.can_dlc; ++i)
        {
            printf("%02x", (int)frame.data[i]);
        }
        cout << endl;



    }
}
