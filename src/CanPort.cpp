#include "CanPort.h"

#include <algorithm>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <wblib/utils.h>

#include "log.h"

#define LOG(logger) ::logger.Log() << "[CAN] "

namespace
{
    const auto READ_TIMEOUT_MS = std::chrono::milliseconds(1000); // 1 sec for messages waiting

    class TCanPort: public CAN::IPort
    {
        int Socket;
        std::thread Thread;
        std::atomic_bool Enabled;
        std::mutex HandlersMutex;
        std::vector<CAN::IFrameHandler*> Handlers;

    public:
        TCanPort(const std::string& ifname)
        {
            struct sockaddr_can addr;
            struct ifreq ifr;

            if ((Socket = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
                throw std::runtime_error("Error while opening CAN socket");
            }

            strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
            ifr.ifr_name[IFNAMSIZ - 1] = '\0';
            ifr.ifr_ifindex = if_nametoindex(ifr.ifr_name);
            if (!ifr.ifr_ifindex) {
                throw std::runtime_error(std::string("if_nametoindex failed for interface '") + ifr.ifr_name + "', " +
                                         strerror(errno));
            }

            addr.can_family = AF_CAN;
            addr.can_ifindex = ifr.ifr_ifindex;

            LOG(WBMQTT::Info) << ifname.c_str() << " at index " << ifr.ifr_ifindex;

            if (bind(Socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                throw std::runtime_error("Error in CAN socket bind");
            }

            Enabled.store(true);

            Thread = std::thread([&]() {
                WBMQTT::SetThreadName("CAN listener");
                while (Enabled.load()) {
                    timeval tv;
                    tv.tv_sec = READ_TIMEOUT_MS.count() / 1000;
                    tv.tv_usec = (READ_TIMEOUT_MS.count() % 1000) * 1000;
                    fd_set rfds;
                    FD_ZERO(&rfds);
                    FD_SET(Socket, &rfds);
                    int r = select(Socket + 1, &rfds, nullptr, nullptr, &tv);
                    if (r < 0) {
                        LOG(WBMQTT::Error) << "select() failed " << strerror(errno);
                        exit(1);
                    }
                    if (r > 0) {
                        CAN::TFrame frame{0};
                        auto nread = read(Socket, &frame, sizeof(CAN::TFrame));
                        if (nread == sizeof(CAN::TFrame)) {
                            std::unique_lock<std::mutex> lk(HandlersMutex);
                            for (auto& handler: Handlers) {
                                try {
                                    if (handler->Handle(frame)) {
                                        break;
                                    }
                                } catch (const std::exception& e) {
                                    LOG(WBMQTT::Error) << e.what();
                                }
                            }
                        } else {
                            if (nread < 0) {
                                LOG(WBMQTT::Error) << "read() failed " << strerror(errno);
                                exit(1);
                            }
                            LOG(WBMQTT::Error) << "Got " << nread << " instead of " << sizeof(CAN::TFrame) << " bytes";
                        }
                    }
                }
            });
        }

        ~TCanPort()
        {
            Enabled.store(false);
            if (Thread.joinable()) {
                Thread.join();
            }
            close(Socket);
        }

        void AddHandler(CAN::IFrameHandler* handler)
        {
            std::unique_lock<std::mutex> lk(HandlersMutex);
            Handlers.push_back(handler);
        }

        void RemoveHandler(CAN::IFrameHandler* handler)
        {
            std::unique_lock<std::mutex> lk(HandlersMutex);
            Handlers.erase(std::remove(Handlers.begin(), Handlers.end(), handler), Handlers.end());
        }

        void Send(const CAN::TFrame& frame)
        {
            auto nbytes = write(Socket, &frame, CAN_MTU);

            if (nbytes < static_cast<int>(CAN_MTU)) {
                throw std::runtime_error(std::string("CAN write error: ") + strerror(errno));
            }
        }
    };
}

std::shared_ptr<CAN::IPort> CAN::MakePort(const std::string& ifname)
{
    return std::make_shared<TCanPort>(ifname);
}
