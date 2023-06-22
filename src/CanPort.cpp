#include "CanPort.h"

#include <algorithm>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <queue>
#include <condition_variable>
#include <wblib/utils.h>

#include "log.h"

#define LOG(logger) ::logger.Log() << "[CAN] "

namespace
{
    const auto READ_TIMEOUT_MS = std::chrono::milliseconds(1000); // 1 sec for messages waiting
    const auto WRITE_TIMEOUT = std::chrono::seconds(5); // 5 sec wait for port ready to write

    template<class TDuration> void setTimeval(timeval& tv, TDuration timeout)
    {
        tv.tv_sec = std::chrono::ceil<std::chrono::seconds>(timeout).count();
        tv.tv_usec = (std::chrono::ceil<std::chrono::microseconds>(timeout).count() % 1000) * 1000;
    }

    class TCanPort: public CAN::IPort
    {
        int Socket;
        std::thread ReadThread;
        std::atomic_bool Enabled;
        std::mutex HandlersMutex;
        std::mutex WriteMutex;
        std::vector<CAN::IFrameHandler*> Handlers;

        std::mutex WriteConfirmMutex;
        std::condition_variable WriteConfirmCv;
        bool Confirmed = false;

        std::thread DispatchThread;
        std::mutex CanDispatchMutex;
        std::condition_variable CanDispatchCv;
        std::queue<CAN::TFrame> CanFrames;

    public:
        TCanPort(const std::string& ifname)
        {
            struct sockaddr_can addr;
            struct ifreq ifr;

            if ((Socket = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
                throw std::runtime_error("Error while opening CAN socket");
            }

            int enable = 1;
            setsockopt(Socket, SOL_CAN_RAW, CAN_RAW_LOOPBACK, &enable, sizeof(enable));
            setsockopt(Socket, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &enable, sizeof(enable));

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

            ReadThread = std::thread([&]() {
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
                        can_frame frame{0};
                        frame.can_dlc = 1;
                        uint8_t ctrlmsg[CMSG_SPACE(sizeof(struct timeval)) + CMSG_SPACE(sizeof(__u32))];
                        iovec iov{0};
                        iov.iov_base = &frame;
                        msghdr msg{0};
                        msg.msg_iov = &iov;
                        msg.msg_iovlen = 1;
                        msg.msg_control = &ctrlmsg;

                        msg.msg_iov[0].iov_len = sizeof(frame);
                        msg.msg_controllen = sizeof(ctrlmsg);
                        msg.msg_flags = 0;

                        auto nread = recvmsg(Socket, &msg, 0);
                        if (nread == sizeof(CAN::TFrame)) {
                            if (msg.msg_flags & MSG_CONFIRM) {
                                std::unique_lock<std::mutex> waitLock(WriteConfirmMutex);
                                Confirmed = true;
                                WriteConfirmCv.notify_all();
                            } else {
                                std::unique_lock<std::mutex> waitLock(CanDispatchMutex);
                                CanFrames.push(frame);
                                CanDispatchCv.notify_all();
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


            DispatchThread = std::thread([&]() {
                WBMQTT::SetThreadName("CAN dispatch");

                CAN::TFrame frame;
                while (Enabled.load()) {
                    {
                        std::unique_lock<std::mutex> waitLock(CanDispatchMutex);
                        if (CanFrames.empty()) {
                            if (std::cv_status::timeout == CanDispatchCv.wait_for(waitLock, READ_TIMEOUT_MS)) {
                                continue;
                            }
                        }
                        frame = CanFrames.front();
                        CanFrames.pop();
                    }
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
                }
            });
        }

        ~TCanPort()
        {
            Enabled.store(false);
            if (ReadThread.joinable()) {
                ReadThread.join();
            }
            if (DispatchThread.joinable()) {
                DispatchThread.join();
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
            std::unique_lock<std::mutex> lk(WriteMutex);
            {
                std::unique_lock<std::mutex> waitLock(WriteConfirmMutex);
                Confirmed = false;
            }

            auto nbytes = write(Socket, &frame, CAN_MTU);

            if (nbytes < static_cast<int>(CAN_MTU)) {
                throw std::runtime_error(std::string("CAN write error: ") + strerror(errno));
            }
            std::unique_lock<std::mutex> waitLock(WriteConfirmMutex);
            if (Confirmed) {
                return;
            }
            if (std::cv_status::timeout == WriteConfirmCv.wait_for(waitLock, WRITE_TIMEOUT)) {
                throw std::runtime_error("CAN write timeout");
            }
        }
    };
}

std::shared_ptr<CAN::IPort> CAN::MakePort(const std::string& ifname)
{
    return std::make_shared<TCanPort>(ifname);
}
