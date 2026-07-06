#pragma once

#include "async_image_saver.h"
#include "ui.h"

#include <imgui.h>
#include <GL/gl.h>
#include <SDL2/SDL.h>

#include <chrono>

#include "platform/keyboard_listener.h"

/**
 * @brief 实现 UiContext 的平台资源管理与逐帧渲染
 *
 * @details 该内部类型统一管理 SDL 窗口、OpenGL 上下文、ImGui 后端、纹理、
 * 输入服务和异步保存服务，并维护跨渲染帧的缓存状态
 */
class UiContext::Impl
{
public:

    /**
     * @brief 关闭后台服务并按依赖顺序释放 UI 资源
     */
    ~Impl();

    /**
     * @brief 初始化窗口、渲染后端、字体和输入服务
     *
     * @param title SDL 窗口标题
     * @param state 接收持久化控制面板设置的应用状态
     *
     * @throws std::logic_error 对已初始化对象重复调用
     * @throws std::runtime_error 平台窗口、OpenGL 上下文或 ImGui 后端初始化失败
     */
    void init(const char *title, AppState &state);

    /**
     * @brief 保存界面设置并释放全部平台资源
     *
     * @details 可重复调用；资源按纹理、ImGui、OpenGL 上下文和窗口的顺序释放
     */
    void shutdown();

    /**
     * @brief 轮询 SDL 与终端键盘事件
     *
     * @param state 接收退出、快捷键和交互请求的应用状态
     */
    void processEvents(AppState &state);

    /**
     * @brief 更新待显示内容并提交一帧 ImGui 渲染
     *
     * @param state 提供源帧和交互状态，并接收 UI 操作请求
     *
     * @return 本次刷新是否展示了新的非暂停源帧
     *
     * @throws std::runtime_error 字体纹理重建失败
     */
    bool updateAndRender(AppState &state);

    /**
     * @brief 清除切换数据源后不可复用的纹理和派生状态
     *
     * @param state 已切换到新 ZMQ 端点或本地视频的应用状态
     */
    void onEndpointChanged(AppState &state);

private:

    /**
     * @brief 记录 OpenGL 纹理对象及其已分配尺寸
     */
    struct Texture
    {
        GLuint id = 0; ///< OpenGL 对象名称，零表示尚未创建
        int width = 0; ///< 已分配纹理存储区的像素宽度
        int height = 0; ///< 已分配纹理存储区的像素高度
    };

    // === 窗口与渲染上下文 ===
    SDL_Window *window_ = nullptr; ///< 实现对象拥有的 SDL 顶层窗口
    SDL_GLContext glContext_ = nullptr; ///< 创建在 window_ 上的 OpenGL 上下文
    bool vsyncEnabled_ = true; ///< 缓冲区交换是否由垂直同步控制节拍

    // === 帧与直方图显示 ===
    Texture frameTex_; ///< 最近生成的显示帧纹理
    Texture histogramTex_; ///< 最近生成的直方图纹理

    // === 后台服务 ===
    KeyboardListener keyboard_; ///< 终端快捷键采集与事件队列
    AsyncImageSaver imageSaver_; ///< 在工作线程执行图像编码和文件写入
    bool keyboardListening_ = false; ///< 终端键盘服务是否成功启动
    SDL_Keycode heldVideoSeekKey_ = SDLK_UNKNOWN; ///< 当前按住的视频逐帧方向键
    Uint32 videoSeekStartedAt_ = 0; ///< 当前方向键开始按下的 SDL 毫秒时间戳

    // === 字体状态 ===
    float uiFontSize_ = 32.0F; ///< 用户请求的字体像素尺寸
    float loadedFontSize_ = 32.0F; ///< 当前字体图集使用的像素尺寸
    bool fontRebuildPending_ = false; ///< 是否存在等待防抖的字体重建请求
    std::chrono::steady_clock::time_point fontRebuildAt_{}; ///< 字体防抖结束并允许重建的时刻

    // === 渲染节拍 ===
    std::chrono::steady_clock::time_point nextFrameAt_{}; ///< 软件限帧模式计划提交下一帧的时刻
    std::chrono::steady_clock::time_point lastRenderTime_{}; ///< 上一帧时刻，用于计算 ImGui DeltaTime

    // === 持久化状态 ===
    float controlRatio_ = 0.25F; ///< 退出时写入配置的控制面板宽度比例

    /**
     * @brief 准备 OpenGL 视口并开始新的 ImGui 帧
     *
     * @param deltaTime 距上一渲染帧的秒数
     */
    void beginFrame(float deltaTime);

    /**
     * @brief 提交 ImGui 绘制数据并交换窗口缓冲区
     *
     * @details 窗口最小化时跳过绘制；VSync 不可用时执行软件限帧
     */
    void endFrame();

    /**
     * @brief 更新目标字号并安排防抖后的字体图集重建
     *
     * @param size 用户请求的字体像素尺寸
     */
    void setFontSize(float size);

    void getWindowSize(int &width, int &height) const;

    /**
     * @brief 将单个 SDL 事件分发给 ImGui 或应用快捷键处理器
     *
     * @param event 当前 SDL 事件
     * @param state 接收事件结果的应用状态
     */
    void processSdlEvent(const SDL_Event &event, AppState &state);

    void uploadFrameTexture(const cv::Mat &frameBgr);

    void uploadHistogramTexture(const cv::Mat &histBgr);

    void clearFrameTexture();

    void clearHistogramTexture();

    void draw(AppState &state);

    /**
     * @brief 在保存请求需要时延迟生成直方图图像
     *
     * @param state 提供源帧并接收直方图结果
     */
    static void ensureHistogramFrame(AppState &state);

    /**
     * @brief 提交待保存图像并轮询异步保存结果
     *
     * @param state 提供保存请求、图像数据并接收结果文本
     */
    void handleSaveRequests(AppState &state);

    /**
     * @brief 在显示依赖项变化时重新生成并上传帧纹理
     *
     * @param state 提供源帧、显示模式和分析参数
     *
     * @return 本次刷新是否展示了新的非暂停源帧
     */
    bool updateFrameTexture(AppState &state);

    /**
     * @brief 根据可见性、数据版本和节流状态维护直方图纹理
     *
     * @param state 提供源帧、ROI 和直方图模式
     * @param analysisDirty 分析依赖项是否需要立即刷新
     */
    void updateHistogramTexture(AppState &state, bool analysisDirty);

    /**
     * @brief 持久化窗口尺寸、字号和控制面板比例
     */
    void saveSettings() const;

    /**
     * @brief 清空并重新构建 ImGui 字体图集
     *
     * @param size 字体图集使用的像素尺寸
     */
    static void loadFonts(float size);

    /**
     * @brief 在防抖期限到达后同步重建字体与 OpenGL 字体纹理
     *
     * @throws std::runtime_error OpenGL 字体纹理创建失败
     */
    void rebuildFontsIfDue();

    /**
     * @brief 创建或更新 BGR 图像对应的 OpenGL 纹理
     *
     * @param texture 接收纹理对象和尺寸状态
     * @param bgr 待上传的 CV_8UC3 图像
     *
     * @throws std::invalid_argument 图像类型不是 CV_8UC3
     */
    static void uploadTexture(Texture &texture, const cv::Mat &bgr);

    /**
     * @brief 释放 OpenGL 纹理并清空其尺寸状态
     *
     * @param texture 待释放的纹理状态
     */
    static void destroyTexture(Texture &texture);

    /**
     * @brief 计算图像在自动适配或手动缩放模式下的显示尺寸
     *
     * @param image 待显示图像
     * @param available 视口可用尺寸
     * @param state 提供自动适配开关和缩放倍率
     *
     * @return 受缩放边界约束的显示尺寸
     */
    [[nodiscard]] ImVec2 scaledImageSize(const cv::Mat &image, const ImVec2 &available,
        const AppState &state) const;

    /**
     * @brief 绘制连接、显示、分析、历史浏览和保存控件
     *
     * @param state 提供当前状态并接收用户操作请求
     * @param displaySize 当前 ImGui 显示区域尺寸
     */
    void drawControlPanel(AppState &state, const ImVec2 &displaySize);

    /**
     * @brief 绘制并处理控制面板宽度分隔条
     *
     * @param state 提供折叠与拖动状态，并接收新的宽度比例
     * @param displaySize 当前 ImGui 显示区域尺寸
     */
    void drawControlSplitter(AppState &state, const ImVec2 &displaySize);

    /**
     * @brief 绘制图像视口、像素状态栏和可选直方图窗口
     *
     * @param state 提供纹理关联状态，并接收 ROI、缩放和平移交互
     * @param displaySize 当前 ImGui 显示区域尺寸
     */
    void drawImageWindow(AppState &state, const ImVec2 &displaySize);

    /**
     * @brief 记录 GPU 纹理内容版本和直方图刷新节拍
     */
    struct PerFrameState
    {
        uint64_t displayedRevision = 0; ///< 已上传到 frameTex_ 的源图像版本
        ViewMode displayedMode = ViewMode::Color; ///< 生成 frameTex_ 时采用的视图模式
        bool histogramTextureReady = false; ///< histogramTex_ 当前是否包含可显示数据
        uint64_t histogramRevision = 0; ///< 生成 histogramTex_ 时采用的源图像版本
        ViewMode histogramMode = ViewMode::Color; ///< 生成 histogramTex_ 时采用的视图模式
        std::chrono::steady_clock::time_point lastHistogramUpdate{}; ///< 最近一次直方图纹理更新时间
    };

    // === 跨帧缓存 ===
    PerFrameState pf_; ///< 跨渲染帧保留的纹理版本缓存
};
