#include <wbmqtt/wbmqtt.h>

#include <iostream>
#include <cstdio>
#include <getopt.h>
#include <unistd.h>

#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <linux/can.h>
#include <linux/can/raw.h>


using namespace WBMQTT;
using namespace std;


const auto DRIVER_NAME       = "wb-mqtt-can";
const auto LIBWBMQTT_DB_FILE = "/tmp/libwbmqtt.db";


void print_usage_and_exit()
{
    printf("Usage:\n wb-mqtt-can -c <config file>\n");
    exit(-1);
}

int connect_can()
{
    int s;
	struct sockaddr_can addr;
	struct ifreq ifr;

	const char *ifname = "vcan0";

	if((s = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
		perror("Error while opening socket");
		exit(1);
	}

	strcpy(ifr.ifr_name, ifname);
	ioctl(s, SIOCGIFINDEX, &ifr);

	addr.can_family  = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;

	printf("%s at index %d\n", ifname, ifr.ifr_ifindex);

	if(bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("Error in socket bind");
		exit(2);
	}

    return s;
}

int main(int argc, char *argv[])
{
    string config_fname;
    TMosquittoMqttConfig mqttConfig;
    bool debug = false;

    int c;

    while ( (c = getopt(argc, argv, "c:h:p:")) != -1) {
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


}
