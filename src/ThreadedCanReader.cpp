#include "ThreadedCanReader.h"

#include <cstring>
#include <wblib/utils.h>

namespace
{
    const auto READ_TIMEOUT = std::chrono::milliseconds(1000);
}

TThreadedCanReader::TThreadedCanReader(const std::string& threadName,
                                       std::shared_ptr<CAN::IPort> canPort,
                                       size_t framesQueueMaxLength,
                                       std::function<bool(const CAN::TFrame& frame)> acceptFrame,
                                       std::function<void(const CAN::TFrame& frame)> handleFrame)
    : CanPort(canPort),
      FramesQueueMaxLength(framesQueueMaxLength),
      AcceptFrame(acceptFrame)
{
    Enabled.store(true);
    CanPort->AddHandler(this);

    Thread = std::thread([this, threadName, handleFrame]() {
        WBMQTT::SetThreadName(threadName);
        CAN::TFrame frame;
        while (Enabled.load()) {
            memset(&frame, 0, sizeof(CAN::TFrame));
            if (Get(frame)) {
                handleFrame(frame);
            }
        }
    });
}

TThreadedCanReader::~TThreadedCanReader()
{
    CanPort->RemoveHandler(this);
    Enabled.store(false);
    if (Thread.joinable()) {
        Thread.join();
    }
}

bool TThreadedCanReader::Handle(const CAN::TFrame& frame)
{
    if (!AcceptFrame(frame)) {
        return false;
    }
    std::unique_lock<std::mutex> waitLock(Mutex);
    if (Frames.size() < FramesQueueMaxLength) {
        Frames.push(frame);
        Cv.notify_all();
    }
    return true;
}

bool TThreadedCanReader::Get(CAN::TFrame& frame)
{
    std::unique_lock<std::mutex> waitLock(Mutex);
    if (Frames.empty()) {
        if (std::cv_status::timeout == Cv.wait_for(waitLock, READ_TIMEOUT)) {
            return false;
        }
    }
    frame = Frames.front();
    Frames.pop();
    return true;
}
