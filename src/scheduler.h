#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ITask: public std::enable_shared_from_this<ITask>
{
public:
    virtual ~ITask() = default;

    virtual std::vector<std::shared_ptr<ITask>> Run() = 0;

    virtual const std::string& GetName() const = 0;

    virtual const std::chrono::microseconds& GetPeriod() const = 0;
};

class IScheduler
{
public:
    virtual ~IScheduler() = default;

    virtual void AddTask(std::shared_ptr<ITask> task) = 0;
};

IScheduler* MakeSimpleThreadedScheduler(const std::string& threadName);

std::shared_ptr<ITask> MakePeriodicTask(const std::chrono::microseconds& period,
                                        std::function<void()> fn,
                                        const std::string& name);