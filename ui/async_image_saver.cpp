#include "async_image_saver.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <opencv2/imgcodecs.hpp>

namespace
{
    constexpr std::size_t kSaveQueueCapacity = 4; ///< 排队与执行中任务的合计上限

    /**
     * @brief 在当前目录分配包含时间和进程内序号的不冲突 PNG 路径
     */
    std::filesystem::path makeSavePath(const std::string &kind)
    {
        static std::atomic_uint64_t sequence = 0;
        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        localtime_r(&time, &local);
        const auto milliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) %
                std::chrono::seconds(1);

        std::ostringstream name;
        name << kind << "_" << std::put_time(&local, "%Y%m%d_%H%M%S") << "_" << std::setw(3)
                << std::setfill('0') << milliseconds.count() << "_" << sequence.fetch_add(1);

        const std::filesystem::path directory = std::filesystem::current_path();
        std::filesystem::path path = directory / (name.str() + ".png");
        for (std::size_t suffix = 1; std::filesystem::exists(path); ++suffix)
        {
            path = directory / (name.str() + "_" + std::to_string(suffix) + ".png");
        }

        return path;
    }
}

AsyncImageSaver::AsyncImageSaver(): worker_([this] { run(); })
{
}

AsyncImageSaver::~AsyncImageSaver()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }

    condition_.notify_one();
    worker_.join();
}

bool AsyncImageSaver::submit(const cv::Mat &image, const std::string &kind,
    const std::string &label)
{
    if (image.empty())
    {
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        if (outstandingTasks_ >= kSaveQueueCapacity)
        {
            results_.emplace_back("Save queue is full");

            return false;
        }
        ++outstandingTasks_;
    }

    Task task;
    try
    {
        task = Task{image.clone(), makeSavePath(kind), label};
    }
    catch (const std::exception &error)
    {
        std::lock_guard lock(mutex_);
        --outstandingTasks_;
        results_.push_back("Save failed: " + std::string(error.what()));

        return false;
    }

    {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
    }

    condition_.notify_one();

    return true;
}

std::optional<std::string> AsyncImageSaver::pollResult()
{
    std::lock_guard lock(mutex_);
    if (results_.empty())
    {
        return std::nullopt;
    }

    std::string result = std::move(results_.front());
    results_.pop_front();

    return result;
}

void AsyncImageSaver::run()
{
    for (;;)
    {
        Task task;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty())
            {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop_front();
        }

        std::string result;
        try
        {
            if (!cv::imwrite(task.path.string(), task.image))
            {
                throw std::runtime_error("image encoder rejected frame");
            }

            result = "Saved " + task.label + ": " + task.path.filename().string();

            std::cout << "Saved " << task.path << '\n';
        }
        catch (const std::exception &error)
        {
            result = "Save failed: " + std::string(error.what());
        }

        std::lock_guard lock(mutex_);
        results_.push_back(std::move(result));
        --outstandingTasks_;
    }
}
