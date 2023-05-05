#include "scheduler.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <wblib/utils.h>

namespace
{

    struct TTaskDescription
    {
        std::chrono::steady_clock::time_point NextRun;
        std::shared_ptr<ITask> Task;

        TTaskDescription(std::chrono::steady_clock::time_point nextRun, std::shared_ptr<ITask> task)
            : NextRun(nextRun),
              Task(task)
        {}
    };

    class TSimpleThreadedScheduler: public IScheduler
    {
        std::thread Thread;

        std::atomic_bool Enabled;
        std::mutex TasksMutex;
        std::vector<TTaskDescription> Tasks;

        std::mutex WaitMutex;
        std::condition_variable ConditionVariable;

        std::string ThreadName;

        void MakeIteration()
        {
            auto now = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> tasksLock(TasksMutex);
            if (Tasks.empty()) {
                tasksLock.unlock();
                std::unique_lock<std::mutex> waitLock(WaitMutex);
                ConditionVariable.wait(waitLock);
                return;
            }
            if (Tasks.begin()->NextRun <= now) {
                TTaskDescription td = *Tasks.begin();
                Tasks.erase(Tasks.begin());
                tasksLock.unlock();
                auto res = td.Task->Run();
                tasksLock.lock();
                for (auto& t: res) {
                    Tasks.emplace_back(now + t->GetPeriod(), t);
                }
                return;
            }
            std::stable_sort(Tasks.begin(), Tasks.end(), [](const auto& t1, const auto& t2) {
                return t1.NextRun < t2.NextRun;
            });
            auto nextRun = Tasks.begin()->NextRun;
            tasksLock.unlock();
            std::unique_lock<std::mutex> waitLock(WaitMutex);
            ConditionVariable.wait_until(waitLock, nextRun);
        }

    public:
        TSimpleThreadedScheduler(const std::string& threadName): Enabled(true), ThreadName(threadName)
        {
            Thread = std::thread([&]() {
                WBMQTT::SetThreadName(ThreadName);
                while (Enabled.load()) {
                    MakeIteration();
                }
            });
        }

        ~TSimpleThreadedScheduler()
        {
            Enabled.store(false);
            ConditionVariable.notify_one();
            Thread.join();
        }

        void AddTask(std::shared_ptr<ITask> task) override
        {
            std::unique_lock<std::mutex> lk(TasksMutex);
            Tasks.emplace(Tasks.begin(), std::chrono::steady_clock::time_point(), task);
            ConditionVariable.notify_one();
        }
    };

    class TPeriodicTask: public ITask
    {
        std::function<void()> Fn;
        std::chrono::microseconds Period;
        std::string Name;

    public:
        TPeriodicTask(const std::chrono::microseconds& period, std::function<void()> fn, const std::string& name)
            : Fn(fn),
              Period(period),
              Name(name)
        {}

        std::vector<std::shared_ptr<ITask>> Run() override
        {
            Fn();
            return {shared_from_this()};
        }

        const std::string& GetName() const override
        {
            return Name;
        }

        const std::chrono::microseconds& GetPeriod() const override
        {
            return Period;
        }
    };
}

IScheduler* MakeSimpleThreadedScheduler(const std::string& threadName)
{
    return new TSimpleThreadedScheduler(threadName);
}

std::shared_ptr<ITask> MakePeriodicTask(const std::chrono::microseconds& period,
                                        std::function<void()> fn,
                                        const std::string& name)
{
    return std::make_shared<TPeriodicTask>(period, fn, name);
}
