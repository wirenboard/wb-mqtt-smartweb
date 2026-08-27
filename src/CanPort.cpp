#include "CanPort.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <net/if.h>
#include <queue>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wblib/utils.h>

#include "log.h"

#define LOG(logger) ::logger.Log() << "[CAN] "

namespace
{
    const auto READ_TIMEOUT_MS = std::chrono::milliseconds(1000); // 1 sec for messages waiting
    const auto WRITE_TIMEOUT = std::chrono::seconds(5);           // 5 sec wait for port ready to write
    const auto RECONNECT_INTERVAL = std::chrono::seconds(1);

    template<class TDuration> void setTimeval(timeval& tv, TDuration timeout)
    {
        tv.tv_sec = std::chrono::ceil<std::chrono::seconds>(timeout).count();
        tv.tv_usec = (std::chrono::ceil<std::chrono::microseconds>(timeout).count() % 1000) * 1000;
    }

    void initMsghdr(msghdr& msg, can_frame& frame, iovec& iov, uint8_t* ctrlmsg, size_t ctrlmsgSize)
    {
        iov.iov_base = &frame;
        iov.iov_len = sizeof(frame);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrlmsg;
        msg.msg_controllen = ctrlmsgSize;
        msg.msg_flags = 0;
    }

    class TCanPort: public CAN::IPort
    {
        int Socket = -1;
        std::string IfName;
        std::thread Thread;
        std::atomic_bool Enabled;
        std::atomic_bool Connected{false};
        std::mutex HandlersMutex;
        std::mutex WriteMutex;
        std::vector<CAN::IFrameHandler*> Handlers;

        std::mutex WriteConfirmMutex;
        std::condition_variable WriteConfirmCv;
        bool WriteConfirmed = false;

        void SetWriteConfirmed()
        {
            std::unique_lock<std::mutex> waitLock(WriteConfirmMutex);
            WriteConfirmed = true;
            WriteConfirmCv.notify_all();
        }

        void OpenSocket()
        {
            struct sockaddr_can addr;
            struct ifreq ifr;

            int sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
            if (sock < 0) {
                throw std::runtime_error(std::string("Error while opening CAN socket: ") + strerror(errno));
            }

            int enable = 1;
            setsockopt(sock, SOL_CAN_RAW, CAN_RAW_LOOPBACK, &enable, sizeof(enable));
            setsockopt(sock, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &enable, sizeof(enable));

            strncpy(ifr.ifr_name, IfName.c_str(), IFNAMSIZ - 1);
            ifr.ifr_name[IFNAMSIZ - 1] = '\0';
            ifr.ifr_ifindex = if_nametoindex(ifr.ifr_name);
            if (!ifr.ifr_ifindex) {
                close(sock);
                throw std::runtime_error(std::string("if_nametoindex failed for interface '") + ifr.ifr_name + "', " +
                                         strerror(errno));
            }

            addr.can_family = AF_CAN;
            addr.can_ifindex = ifr.ifr_ifindex;

            if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                close(sock);
                throw std::runtime_error(std::string("Error in CAN socket bind: ") + strerror(errno));
            }

            if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0 || !(ifr.ifr_flags & IFF_UP)) {
                close(sock);
                throw std::runtime_error("CAN interface '" + IfName + "' is down");
            }

            LOG(WBMQTT::Info) << IfName.c_str() << " at index " << ifr.ifr_ifindex;

            {
                std::unique_lock<std::mutex> lk(WriteMutex);
                Socket = sock;
                Connected.store(true);
            }
        }

        void CloseSocket()
        {
            std::unique_lock<std::mutex> lk(WriteMutex);
            Connected.store(false);
            if (Socket >= 0) {
                close(Socket);
                Socket = -1;
            }
        }

        void NotifyConnection(bool connected)
        {
            std::unique_lock<std::mutex> lk(HandlersMutex);
            for (auto& handler: Handlers) {
                handler->OnConnectionChanged(connected);
            }
        }

        void Reconnect()
        {
            bool reported = false;
            while (Enabled.load()) {
                try {
                    OpenSocket();
                    LOG(WBMQTT::Info) << "CAN connection established";
                    return;
                } catch (const std::exception& e) {
                    if (!reported) {
                        LOG(WBMQTT::Warn) << e.what();
                        reported = true;
                    }
                }
                std::this_thread::sleep_for(RECONNECT_INTERVAL);
            }
        }

        void RunHandlers(const CAN::TFrame& frame)
        {
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

        void ThreadFn()
        {
            while (Enabled.load()) {
                if (!Connected.load()) {
                    CloseSocket();
                    NotifyConnection(false);
                    Reconnect();
                    if (Connected.load()) {
                        NotifyConnection(true);
                    }
                    continue;
                }
                timeval tv;
                setTimeval(tv, READ_TIMEOUT_MS);
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(Socket, &rfds);
                int r = select(Socket + 1, &rfds, nullptr, nullptr, &tv);
                if (r < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    LOG(WBMQTT::Error) << "select() failed " << strerror(errno);
                    Connected.store(false);
                    continue;
                }
                if (r > 0) {
                    msghdr msg{0};
                    can_frame frame{0};
                    iovec iov{0};
                    uint8_t ctrlmsg[CMSG_SPACE(sizeof(struct timeval)) + CMSG_SPACE(sizeof(__u32))];
                    initMsghdr(msg, frame, iov, ctrlmsg, sizeof(ctrlmsg));

                    auto nread = recvmsg(Socket, &msg, 0);
                    if (nread == sizeof(CAN::TFrame)) {
                        if (msg.msg_flags & MSG_CONFIRM) {
                            SetWriteConfirmed();
                        } else {
                            RunHandlers(frame);
                        }
                    } else {
                        if (nread < 0) {
                            LOG(WBMQTT::Error) << "read() failed " << strerror(errno);
                            Connected.store(false);
                            continue;
                        }
                        LOG(WBMQTT::Error) << "Got " << nread << " instead of " << sizeof(CAN::TFrame) << " bytes";
                    }
                }
            }
        }

    public:
        TCanPort(const std::string& ifname): IfName(ifname)
        {
            Enabled.store(true);

            Thread = std::thread([this]() {
                WBMQTT::SetThreadName("CAN listener");
                ThreadFn();
            });
        }

        ~TCanPort()
        {
            Enabled.store(false);
            if (Thread.joinable()) {
                Thread.join();
            }
            CloseSocket();
        }

        void AddHandler(CAN::IFrameHandler* handler) override
        {
            std::unique_lock<std::mutex> lk(HandlersMutex);
            Handlers.push_back(handler);
            handler->OnConnectionChanged(Connected.load());
        }

        void RemoveHandler(CAN::IFrameHandler* handler) override
        {
            std::unique_lock<std::mutex> lk(HandlersMutex);
            Handlers.erase(std::remove(Handlers.begin(), Handlers.end(), handler), Handlers.end());
        }

        void Send(const CAN::TFrame& frame) override
        {
            std::unique_lock<std::mutex> lk(WriteMutex);
            if (!Connected.load()) {
                throw std::runtime_error("CAN port is disconnected");
            }
            {
                std::unique_lock<std::mutex> waitLock(WriteConfirmMutex);
                WriteConfirmed = false;
            }

            auto nbytes = write(Socket, &frame, CAN_MTU);

            if (nbytes < static_cast<int>(CAN_MTU)) {
                auto err = errno;
                if (err == ENETDOWN || err == ENODEV || err == ENXIO || err == EBADF) {
                    Connected.store(false);
                }
                throw std::runtime_error(std::string("CAN write error: ") + strerror(err));
            }
            std::unique_lock<std::mutex> waitLock(WriteConfirmMutex);
            if (WriteConfirmed) {
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
