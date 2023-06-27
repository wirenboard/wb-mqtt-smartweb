#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

#include "CanPort.h"

class TThreadedCanReader: public CAN::IFrameHandler
{
    std::shared_ptr<CAN::IPort> CanPort;

    std::mutex Mutex;
    std::condition_variable Cv;
    std::queue<CAN::TFrame> Frames;
    size_t FramesQueueMaxLength;

    std::thread Thread;
    std::atomic_bool Enabled;

    std::function<bool(const CAN::TFrame& frame)> AcceptFrame;

    bool Handle(const CAN::TFrame& frame);
    bool Get(CAN::TFrame& frame);

public:
    TThreadedCanReader(const std::string& threadName,
                       std::shared_ptr<CAN::IPort> canPort,
                       size_t framesQueueMaxLength,
                       std::function<bool(const CAN::TFrame& frame)> acceptFrame,
                       std::function<void(const CAN::TFrame& frame)> handleFrame);
    ~TThreadedCanReader();
};
