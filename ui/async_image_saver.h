#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <opencv2/core/mat.hpp>

/**
 * @brief 使用单个后台线程编码并保存图像
 *
 * @details 提交时复制图像并限制未完成任务数量，保存结果通过非阻塞轮询返回
 */
class AsyncImageSaver
{
public:

    /**
     * @brief 启动后台保存线程
     */
    AsyncImageSaver();

    /**
     * @brief 完成已接收任务并回收后台线程
     */
    ~AsyncImageSaver();

    /**
     * @brief 后台线程和同步对象不支持复制
     */
    AsyncImageSaver(const AsyncImageSaver &) = delete;

    /**
     * @brief 后台线程和同步对象不支持复制赋值
     */
    AsyncImageSaver &operator=(const AsyncImageSaver &) = delete;

    /**
     * @brief 将图像副本加入后台保存队列
     *
     * @param image 待保存图像，成功提交时会克隆其数据
     * @param kind 用于构造文件名的图像类别
     * @param label 用于构造结果消息的显示名称
     *
     * @return 任务成功入队时返回 true，图像为空、容量已满或任务构造失败时返回 false
     */
    bool submit(const cv::Mat &image, const std::string &kind, const std::string &label);

    /**
     * @brief 非阻塞获取最早完成的保存结果
     *
     * @return 有结果时返回状态文本，结果队列为空时返回 std::nullopt
     */
    std::optional<std::string> pollResult();

private:

    /**
     * @brief 后台线程消费的完整保存任务
     */
    struct Task
    {
        cv::Mat image; ///< 与调用方生命周期独立的图像数据
        std::filesystem::path path; ///< 已分配的输出文件路径
        std::string label; ///< 写入结果消息使用的图像名称
    };

    void run();

    // === 保存任务调度 ===
    std::mutex mutex_; ///< 保护队列、计数器和停止状态
    std::condition_variable condition_; ///< 唤醒等待任务或退出信号的工作线程
    std::deque<Task> tasks_; ///< 尚未开始写入的任务
    std::deque<std::string> results_; ///< 等待前台读取的完成消息
    std::size_t outstandingTasks_ = 0; ///< 排队中与执行中的任务总数

    // === 工作线程生命周期 ===
    bool stopping_ = false; ///< 是否拒绝新任务并请求工作线程退出
    std::thread worker_; ///< 串行执行编码和文件写入的线程
};
