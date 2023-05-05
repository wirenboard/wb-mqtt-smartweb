#include <wblib/log.h>
#include <wblib/signal_handling.h>
#include <wblib/wbmqtt.h>

#include <getopt.h>

#include "CanPort.h"
#include "MqttToSmartWebGateway.h"
#include "SmartWebToMqttGateway.h"
#include "config_parser.h"
#include "exceptions.h"
#include "log.h"

#define STR(x) #x
#define XSTR(x) STR(x)

using namespace std;
using namespace WBMQTT;

const auto APP_NAME = "wb-mqtt-smartweb";
const auto LIBWBMQTT_DB_FULL_FILE_PATH = "/var/lib/wb-mqtt-smartweb/libwbmqtt.db";
const auto CONFIG_FULL_FILE_PATH = "/etc/wb-mqtt-smartweb.conf";
const auto BUILT_IN_DEVICE_CLASSES_PATH = "/usr/share/wb-mqtt-smartweb/classes";
const auto CONFIG_JSON_SCHEMA_FULL_FILE_PATH = "/usr/share/wb-mqtt-confed/schemas/wb-mqtt-smartweb.schema.json";
const auto CLASS_JSON_SCHEMA_FULL_FILE_PATH = "/usr/share/wb-mqtt-confed/schemas/wb-mqtt-smartweb-class.schema.json";

const auto DRIVER_STOP_TIMEOUT_S = chrono::seconds(10);

//! Maximun time to start application. Exceded timeout will case application termination.
const auto DRIVER_INIT_TIMEOUT_S = chrono::seconds(60);

#define LOG(logger) ::logger.Log() << "[main] "

namespace
{
    void PrintUsage();

    void ApplyDebugParam(int debugLevel)
    {
        switch (debugLevel) {
            case 0:
                break;
            case -1:
                ErrorMqttToSw.SetEnabled(false);
                WarnMqttToSw.SetEnabled(false);
                InfoMqttToSw.SetEnabled(false);
                DebugMqttToSw.SetEnabled(false);
                break;
            case -2:
                ErrorSwToMqtt.SetEnabled(false);
                WarnSwToMqtt.SetEnabled(false);
                InfoSwToMqtt.SetEnabled(false);
                DebugSwToMqtt.SetEnabled(false);
                break;
            case -3:
                WBMQTT::Info.SetEnabled(false);
                break;
            case -4:
                ErrorMqttToSw.SetEnabled(false);
                WarnMqttToSw.SetEnabled(false);
                InfoMqttToSw.SetEnabled(false);
                DebugMqttToSw.SetEnabled(false);
                ErrorSwToMqtt.SetEnabled(false);
                WarnSwToMqtt.SetEnabled(false);
                InfoSwToMqtt.SetEnabled(false);
                DebugSwToMqtt.SetEnabled(false);
                WBMQTT::Info.SetEnabled(false);
                break;
            case 1:
                DebugMqttToSw.SetEnabled(true);
                break;
            case 2:
                DebugSwToMqtt.SetEnabled(true);
                break;
            case 3:
                WBMQTT::Debug.SetEnabled(true);
                break;
            case 4:
                DebugMqttToSw.SetEnabled(true);
                DebugSwToMqtt.SetEnabled(true);
                WBMQTT::Debug.SetEnabled(true);
                break;
            default:
                cout << "Invalid -d parameter value " << debugLevel << endl;
                PrintUsage();
                exit(2);
        }
    }

    void PrintStartupInfo()
    {
        std::string commit(XSTR(WBMQTT_COMMIT));
        cout << APP_NAME << " " << XSTR(WBMQTT_VERSION);
        if (!commit.empty()) {
            cout << " git " << commit;
        }
        cout << endl;
    }

    void PrintUsage()
    {
        PrintStartupInfo();
        cout << "Usage:" << endl
             << " " << APP_NAME << " [options]" << endl
             << "Options:" << endl
             << "  -d      level      enable debuging output:" << endl
             << "                       1 - MQTT to SmartWeb only;" << endl
             << "                       2 - SmartWeb to MQTT only;" << endl
             << "                       3 - MQTT only;" << endl
             << "                       4 - all;" << endl
             << "                       negative values - silent mode (-1, -2, -3, -4))" << endl
             << "  -c      config     config file (default " << CONFIG_FULL_FILE_PATH << ")" << endl
             << "  -i      interface  CAN interface (default: can0)" << endl
             << "  -p      port       MQTT broker port (default: 1883)" << endl
             << "  -h, -H  IP         MQTT broker IP (default: localhost)" << endl
             << "  -u      user       MQTT user (optional)" << endl
             << "  -P      password   MQTT user password (optional)" << endl
             << "  -T      prefix     MQTT topic prefix (optional)" << endl;
    }

    void ParseCommadLine(int argc, char* argv[], TMosquittoMqttConfig& mqttConfig, string& configFile, string& ifname)
    {
        int c;

        while ((c = getopt(argc, argv, "d:c:g:p:h:H:T:u:P:")) != -1) {
            switch (c) {
                case 'd':
                    ApplyDebugParam(stoi(optarg));
                    break;
                case 'c':
                    configFile = optarg;
                    break;
                case 'p':
                    mqttConfig.Port = stoi(optarg);
                    break;
                case 'h':
                case 'H':
                    mqttConfig.Host = optarg;
                    break;
                case 'T':
                    mqttConfig.Prefix = optarg;
                    break;
                case 'u':
                    mqttConfig.User = optarg;
                    break;
                case 'P':
                    mqttConfig.Password = optarg;
                    break;
                case 'i':
                    ifname = optarg;
                    break;
                default:
                    PrintUsage();
                    exit(2);
            }
        }
    }
}

int main(int argc, char* argv[])
{
    string configFile(CONFIG_FULL_FILE_PATH);
    string ifname("can0");
    TConfig config;

    TPromise<void> initialized;
    SignalHandling::Handle({SIGINT, SIGTERM});
    SignalHandling::OnSignals({SIGINT, SIGTERM}, [&] { SignalHandling::Stop(); });
    SetThreadName(APP_NAME);
    ParseCommadLine(argc, argv, config.Mqtt, configFile, ifname);

    PrintStartupInfo();

    SignalHandling::SetWaitFor(DRIVER_INIT_TIMEOUT_S, initialized.GetFuture(), [&] {
        LOG(WBMQTT::Error) << "Driver takes too long to initialize. Exiting.";
        exit(1);
    });

    SignalHandling::SetOnTimeout(DRIVER_STOP_TIMEOUT_S, [&] {
        LOG(WBMQTT::Error) << "Driver takes too long to stop. Exiting.";
        exit(1);
    });

    try {
        LoadConfig(config,
                   configFile,
                   configFile + ".d/classes",
                   BUILT_IN_DEVICE_CLASSES_PATH,
                   CONFIG_JSON_SCHEMA_FULL_FILE_PATH,
                   CLASS_JSON_SCHEMA_FULL_FILE_PATH);
        if (config.Debug) {
            ::Debug.SetEnabled(true);
        }
        if (config.Mqtt.Id.empty())
            config.Mqtt.Id = APP_NAME;
    } catch (const exception& e) {
        LOG(WBMQTT::Error) << "FATAL: " << e.what();
        return 1;
    }

    try {
        auto mqtt = NewMosquittoMqttClient(config.Mqtt);
        auto backend = NewDriverBackend(mqtt);
        auto driver = NewDriver(TDriverArgs{}
                                    .SetId(APP_NAME)
                                    .SetBackend(backend)
                                    .SetUseStorage(true)
                                    .SetReownUnknownDevices(true)
                                    .SetStoragePath(LIBWBMQTT_DB_FULL_FILE_PATH));

        driver->StartLoop();
        driver->WaitForReady();

        auto port = CAN::MakePort(ifname);

        {
            TSmartWebToMqttGateway smartWebToMqttGateway(config.SmartWebToMqtt, port, driver);
            std::vector<std::shared_ptr<TMqttToSmartWebGateway>> mqttToSmartWebGateways;
            for (const auto& controller: config.Controllers) {
                mqttToSmartWebGateways.push_back(std::make_shared<TMqttToSmartWebGateway>(controller, port, driver));
            }

            initialized.Complete();
            SignalHandling::Start();
            SignalHandling::Wait();
        }
        driver->StopLoop();
        driver->Close();
    } catch (const exception& e) {
        LOG(WBMQTT::Error) << "FATAL: " << e.what();
        return 1;
    }

    return 0;
}
