#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>

/**
 * @brief 视口从源图像生成显示图像的颜色或通道模式
 */
enum class ViewMode
{
    Color, ///< 保留源图像的 BGR 彩色外观
    Gray, ///< 将源图像转换为灰度视图
    B, ///< 仅显示 BGR 蓝色分量
    G, ///< 仅显示 BGR 绿色分量
    R, ///< 仅显示 BGR 红色分量
    Hsv, ///< 将源图像转换为 HSV 彩色可视化
    H, ///< 仅显示 HSV 色相分量
    S, ///< 仅显示 HSV 饱和度分量
    V ///< 仅显示 HSV 明度分量
};

/**
 * @brief 直方图计数到绘图高度的映射方式
 */
enum class HistogramMode
{
    Off, ///< 关闭直方图计算和窗口
    Linear, ///< 直接按像素计数线性缩放
    Log, ///< 对计数取对数后缩放
    Cumulative ///< 绘制从低值到当前 bin 的累计比例
};

/**
 * @brief 直方图离屏图像与有效绘图区尺寸
 */
namespace HistogramLayout
{
    inline constexpr int kWidth = 900; ///< 生成纹理的完整像素宽度
    inline constexpr int kHeight = 280; ///< 生成纹理的完整像素高度
    inline constexpr int kLeftMargin = 56; ///< 为纵轴标签保留的左边距
    inline constexpr int kRightMargin = 22; ///< 绘图区右侧留白
    inline constexpr int kTopMargin = 42; ///< 为标题和峰值信息保留的上边距
    inline constexpr int kBottomMargin = 42; ///< 为横轴标签保留的下边距
    inline constexpr int kPlotWidth = kWidth - kLeftMargin - kRightMargin; ///< bin 曲线可用宽度
    inline constexpr int kPlotHeight = kHeight - kTopMargin - kBottomMargin; ///< 计数曲线可用高度
}

/**
 * @brief 主循环、输入处理和 UI 渲染之间共享的可变状态
 *
 * @details 主循环写入流数据和统计结果，UI 读取这些值并写入一次性操作请求。
 * 该对象仅在应用主线程访问
 */
struct AppState
{
    // === 视口显示 ===
    ViewMode mode = ViewMode::Color; ///< 当前视口和分析工具采用的通道模式
    bool fitToWindow = true; ///< 是否按可用视口自动计算图像缩放比例
    float zoom = 1.0F; ///< 关闭自适应后使用的手动缩放比例
    float panX = 0.0F; ///< 手动视图相对居中位置的水平像素偏移
    float panY = 0.0F; ///< 手动视图相对居中位置的垂直像素偏移
    bool paused = false; ///< 是否停止采用新到达帧作为当前显示帧

    // === 帧图像 ===
    cv::Mat lastFrame; ///< 当前用于显示和分析的解码原始帧
    cv::Mat lastViewFrame; ///< 当前模式和曝光标记处理后的显示帧
    cv::Mat lastHistogramFrame; ///< 最近一次生成的直方图离屏图像
    uint64_t imageRevision = 0; ///< lastFrame 内容变化时递增的纹理失效版本

    // === 连接与端点 ===
    std::string endpoint = "tcp://127.0.0.1:5555"; ///< 当前 SUB 套接字连接的端点
    std::array<char, 256> endpointInput{}; ///< ImGui 端点输入框持有的可写缓冲区
    std::string requestedEndpoint; ///< 等待主循环尝试连接的新端点
    bool endpointChangeRequested = false; ///< 是否存在尚未处理的端点切换请求
    bool endpointInputResetRequested = false; ///< 是否需要用有效端点覆盖输入框内容
    std::string connectionStatus; ///< 根据连接时间和帧龄生成的状态文本

    // === 本地视频 ===
    std::string videoFilePath; ///< 当前播放的本地视频路径；为空时使用 ZMQ 输入
    std::string requestedVideoFile; ///< SDL 文件拖放提交、等待主循环打开的视频路径
    bool videoFileOpenRequested = false; ///< 是否存在尚未处理的本地视频打开请求
    int64_t videoFrameCount = 0; ///< 视频可定位的总帧数，无法获知时为 0
    int64_t videoFramePosition = 0; ///< 当前显示帧的零基索引
    int64_t requestedVideoFrame = 0; ///< 进度条或快捷键请求的零基目标帧
    bool videoSeekRequested = false; ///< 是否存在尚未处理的视频定位请求
    bool videoCloseRequested = false; ///< 是否请求关闭本地视频并返回 ZMQ 输入
    float videoPlaybackSpeed = 1.0F; ///< 本地视频相对原始帧率的播放倍速

    // === 性能指标 ===
    int frameCount = 0; ///< 当前端点成功采用的帧总数
    float sourceFps = 0.0F; ///< 元数据报告的发布端统计帧率
    float receiveFps = 0.0F; ///< 最近统计窗口内的接收帧率
    float renderFps = 0.0F; ///< 最近统计窗口内展示新帧的速率
    float frameAgeMs = 0.0F; ///< 当前时刻与最近收帧时刻的毫秒差
    float decodeTimeMs = 0.0F; ///< 最近采用帧的后台解码耗时
    float endToEndLatencyMs = 0.0F; ///< 可比较时钟域下的采集到接收延迟
    float pipelineLatencyMs = 0.0F; ///< 发布元数据报告的管线处理延迟
    float encodeLatencyMs = 0.0F; ///< 发布元数据报告的编码延迟
    bool endToEndLatencyValid = false; ///< endToEndLatencyMs 是否可跨时钟比较

    // === 发布元数据 ===
    uint64_t sourceFrameIndex = 0; ///< 最近采用帧在发布会话内的序号
    uint64_t skippedFrames = 0; ///< 根据相邻源帧序号推导的接收端跳帧数
    uint64_t publisherDroppedFrames = 0; ///< 发布元数据报告的累计丢帧数
    uint64_t lastSourceFrameIndex = 0; ///< 当前发布会话内用于跳帧检测的前一序号
    std::string sourceSessionId; ///< 最近采用帧所属的发布会话标识
    std::chrono::steady_clock::time_point lastFrameTime{}; ///< 最近成功采用一帧的本地时刻

    // === 源锁定 ===
    bool sourceLockEnabled = true; ///< 是否启用源过滤，仅接收指定源的数据
    bool autoLockSource = true; ///< 首次收到帧时自动锁定到该源
    std::string lockedSourceId; ///< 当前锁定的发布源标识；为空时接受所有源
    std::vector<std::string> detectedSources; ///< 锁定模式下发现的所有源标识
    uint64_t filteredFrameCount = 0; ///< 锁定模式下被过滤的其他源帧数
    bool sourceSelectionChangeRequested = false; ///< Camera 控件是否请求重置当前源画面

    // === 流元数据 ===
    std::string topic; ///< 最近采用消息的 ZMQ 主题
    std::string sourceId; ///< 最近采用帧的发布源标识
    std::string payloadInfo; ///< 最近采用帧的负载编码摘要
    std::string pixelFormat; ///< 最近采用帧的 OpenCV 类型文本

    // === 历史浏览 ===
    int historySize = 0; ///< 历史缓存当前可浏览的帧数
    int historyOffset = 0; ///< 从最新帧向过去偏移的浏览索引
    bool previousFrameRequested = false; ///< 是否请求将历史偏移增加一帧
    bool nextFrameRequested = false; ///< 是否请求将历史偏移减少一帧
    bool liveFrameRequested = false; ///< 是否请求回到历史缓存中的最新帧
    bool historySeekRequested = false; ///< historyOffset 是否需要应用到当前帧

    // === 保存 ===
    bool saveRawRequested = false; ///< 是否请求异步保存 lastFrame
    bool saveViewRequested = false; ///< 是否请求异步保存当前显示结果
    bool saveHistogramRequested = false; ///< 是否请求异步保存当前直方图
    std::string saveStatus; ///< 最近一次保存提交或完成结果文本

    // === ROI 区域分析 ===
    bool roiEnabled = false; ///< 是否启用 ROI 选择与区域统计
    bool roiValid = false; ///< ROI 坐标是否形成有效非空区域
    bool roiDragging = false; ///< 是否正在通过鼠标更新 ROI 终点
    bool roiChanged = false; ///< ROI 变化是否尚未触发分析结果刷新
    int roiStartX = 0; ///< 本次 ROI 拖动起点的源图像 X 坐标
    int roiStartY = 0; ///< 本次 ROI 拖动起点的源图像 Y 坐标
    int roiX = 0; ///< 规范化 ROI 左上角的源图像 X 坐标
    int roiY = 0; ///< 规范化 ROI 左上角的源图像 Y 坐标
    int roiWidth = 0; ///< 规范化 ROI 的源图像像素宽度
    int roiHeight = 0; ///< 规范化 ROI 的源图像像素高度
    std::array<double, 3> roiMean{}; ///< ROI 内各 BGR 通道的均值
    std::array<double, 3> roiStdDev{}; ///< ROI 内各 BGR 通道的标准差
    double roiMin = 0.0; ///< ROI 灰度图中的最小像素值
    double roiMax = 0.0; ///< ROI 灰度图中的最大像素值

    // === 曝光警告 ===
    bool showExposureWarnings = false; ///< 是否在显示帧覆盖欠曝和过曝颜色
    int shadowThreshold = 8; ///< 判定欠曝的灰度值上限
    int highlightThreshold = 247; ///< 判定过曝的灰度值下限

    // === 直方图 ===
    HistogramMode histogramMode = HistogramMode::Off; ///< 当前直方图计数显示方式
    bool histogramRefreshRequested = false; ///< 是否强制忽略节流并重算直方图
    bool analysisSettingsChanged = false; ///< 曝光或直方图参数是否使派生结果失效

    // === 直方图缓存 ===
    int histogramCacheMaxBin = 255; ///< 缓存曲线允许查询的最大 bin
    int histogramCacheChannelCount = 0; ///< 缓存曲线实际写入的通道数
    std::array<std::string, 3> histogramCacheLabels{}; ///< 缓存曲线对应的通道名称
    std::array<std::array<int, 256>, 3> histogramCachePixels{}; ///< 各通道逐 bin 原始计数

    // === 直方图悬停 ===
    bool histogramHoverValid = false; ///< 直方图悬停 bin 数据是否可展示
    int histogramHoverBin = 0; ///< 鼠标当前对应的直方图 bin
    int histogramHoverChannelCount = 0; ///< 当前直方图包含的有效通道数
    std::array<std::string, 3> histogramHoverLabels{}; ///< 悬停提示中各通道的名称
    std::array<int, 3> histogramHoverPixels{}; ///< 悬停 bin 中各通道的像素计数

    // === 像素悬停 ===
    bool hoverPixelValid = false; ///< 悬停坐标及颜色采样值是否可展示
    float hoverSampleTimer = 0.0F; ///< 距离上次悬停像素采样经过的秒数
    int hoverX = 0; ///< 最近采样像素的源图像 X 坐标
    int hoverY = 0; ///< 最近采样像素的源图像 Y 坐标
    int hoverB = 0; ///< 最近采样像素的蓝色分量
    int hoverG = 0; ///< 最近采样像素的绿色分量
    int hoverR = 0; ///< 最近采样像素的红色分量
    int hoverH = 0; ///< 最近采样像素的色相分量
    int hoverS = 0; ///< 最近采样像素的饱和度分量
    int hoverV = 0; ///< 最近采样像素的明度分量
    int hoverGray = 0; ///< 最近采样像素的灰度值

    // === 控制面板 ===
    bool quit = false; ///< 是否请求主循环结束
    float controlRatio = 0.25F; ///< 展开状态下控制面板目标宽度比例
    bool controlsCollapsed = false; ///< 控制面板是否仅保留展开按钮
    bool resizingControlWidth = false; ///< 分隔条是否正在捕获鼠标拖动
};

/**
 * @brief 将应用状态中的 ROI 限制到给定图像边界
 *
 * @param frame 提供有效宽高边界的图像
 * @param state 包含 ROI 开关和坐标的应用状态
 *
 * @return ROI 已启用且有效时返回裁剪区域，否则返回整帧区域
 */
cv::Rect selectedRoi(const cv::Mat &frame, const AppState &state);

/**
 * @brief 重新计算当前 ROI 的通道统计量和灰度范围
 *
 * @param state 提供源帧与 ROI，并接收统计结果
 */
void updateRoiStatistics(AppState &state);

/**
 * @brief 管理应用窗口、输入转发和逐帧 ImGui 渲染
 *
 * @details 通过 PIMPL 隐藏 SDL 和 OpenGL 资源。对象支持移动但禁止复制，析构时
 * 自动停止后台服务并释放渲染资源
 */
class UiContext
{
public:

    /** @brief 创建尚未初始化的 UI 上下文 */
    UiContext();

    /** @brief 关闭后台服务并释放窗口及渲染资源 */
    ~UiContext();

    /** @brief UI 上下文拥有平台资源，因此禁止复制 */
    UiContext(const UiContext &) = delete;

    /** @brief UI 上下文拥有平台资源，因此禁止复制赋值 */
    UiContext &operator=(const UiContext &) = delete;

    /** @brief 转移 UI 上下文的实现对象所有权 */
    UiContext(UiContext &&) noexcept;

    /** @brief 释放当前实现并接管另一个 UI 上下文 */
    UiContext &operator=(UiContext &&) noexcept;

    /**
     * @brief 创建窗口、OpenGL 上下文及 ImGui 后端
     *
     * @param title SDL 窗口标题
     * @param state 接收持久化控制面板设置的应用状态
     *
     * @throws std::logic_error 对已初始化上下文重复调用
     * @throws std::runtime_error 窗口、OpenGL 上下文或 ImGui 后端初始化失败
     */
    void init(const char *title, AppState &state) const;

    /**
     * @brief 轮询窗口与终端输入并更新应用请求状态
     *
     * @param state 接收退出、快捷键及交互结果的应用状态
     */
    void processEvents(AppState &state) const;

    /**
     * @brief 更新派生图像和纹理并提交一帧界面
     *
     * @param state 提供当前帧和交互状态，并接收 UI 操作请求
     *
     * @return 本次纹理更新是否展示了新的非暂停帧
     *
     * @throws std::runtime_error 字体纹理重建失败
     */
    bool updateAndRender(AppState &state) const;

    /**
     * @brief 清除不能跨数据源复用的纹理和交互缓存
     *
     * @param state 已切换到新 ZMQ 端点或本地视频的应用状态
     */
    void onEndpointChanged(AppState &state) const;

private:

    class Impl;
    std::unique_ptr<Impl> impl_;
};
