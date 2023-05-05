#pragma once

#include <linux/can.h>
#include <linux/can/raw.h>
#include <memory>

namespace CAN
{
    using TFrame = struct can_frame;
    using TFrameData = decltype(TFrame::data);

    class IFrameHandler
    {
    public:
        virtual ~IFrameHandler() = default;

        /**
         * @brief Must be threadsafe
         *
         * @param frame
         * @return true
         * @return false
         */
        virtual bool Handle(const TFrame& frame) = 0;
    };

    class IPort
    {
    public:
        virtual ~IPort() = default;

        virtual void AddHandler(IFrameHandler* handler) = 0;
        virtual void RemoveHandler(IFrameHandler* handler) = 0;

        /**
         * @brief Must be threadsafe
         *
         * @param frame
         */
        virtual void Send(const TFrame& frame) = 0;
    };

    std::shared_ptr<IPort> MakePort(const std::string& ifname);
}
