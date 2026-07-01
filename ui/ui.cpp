#include "ui.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <GL/gl.h>
#include <SDL2/SDL.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "ui_config.h"
#include "ui_context_impl.h"
#include "ui_image_processing.h"
#include "ui_input.h"
#include "ui_layout.h"

namespace
{
    constexpr float kUiScale = 1.25F; ///< 所有 ImGui 控件尺寸初始化时采用的统一比例
    constexpr auto kJetBrainsMonoPath =
            "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf"; ///< 默认拉丁与数字字体文件
    constexpr auto kNotoSansCjkPath =
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"; ///< 向主图集合并中文字符的字体文件
    constexpr auto kFontRebuildDelay = std::chrono::milliseconds(150); ///< 字号输入停止后再重建图集的防抖窗口
    constexpr auto kFallbackFrameInterval = std::chrono::microseconds(16'667); ///< 软件限帧约 60 FPS 的周期
    constexpr auto kMinimizedFrameInterval = std::chrono::milliseconds(50); ///< 不可渲染时降低轮询频率的周期

    void copyEndpointToInput(AppState &state);

    using ui_detail::applyExposureWarnings;
    using ui_detail::makeDisplayImage;
    using ui_detail::makeHistogram;
    using ui_detail::kMaxZoom;
    using ui_detail::kMinZoom;

    /**
     * @brief 把 SDL 键码转换为 ImGui 导航和文本输入键值
     *
     * @param key SDL 键码
     *
     * @return 支持的 ImGui 键值，当前映射表未覆盖时返回 ImGuiKey_None
     */
    ImGuiKey keycodeToImGuiKey(const SDL_Keycode key)
    {
        if (key >= SDLK_a && key <= SDLK_z)
        {
            return static_cast<ImGuiKey>(ImGuiKey_A + (key - SDLK_a));
        }

        if (key >= SDLK_0 && key <= SDLK_9)
        {
            return static_cast<ImGuiKey>(ImGuiKey_0 + (key - SDLK_0));
        }

        switch (key)
        {
            case SDLK_TAB:
                return ImGuiKey_Tab;
            case SDLK_LEFT:
                return ImGuiKey_LeftArrow;
            case SDLK_RIGHT:
                return ImGuiKey_RightArrow;
            case SDLK_UP:
                return ImGuiKey_UpArrow;
            case SDLK_DOWN:
                return ImGuiKey_DownArrow;
            case SDLK_PAGEUP:
                return ImGuiKey_PageUp;
            case SDLK_PAGEDOWN:
                return ImGuiKey_PageDown;
            case SDLK_HOME:
                return ImGuiKey_Home;
            case SDLK_END:
                return ImGuiKey_End;
            case SDLK_INSERT:
                return ImGuiKey_Insert;
            case SDLK_DELETE:
                return ImGuiKey_Delete;
            case SDLK_BACKSPACE:
                return ImGuiKey_Backspace;
            case SDLK_SPACE:
                return ImGuiKey_Space;
            case SDLK_RETURN:
                return ImGuiKey_Enter;
            case SDLK_ESCAPE:
                return ImGuiKey_Escape;
            case SDLK_LCTRL:
            case SDLK_RCTRL:
                return ImGuiKey_ModCtrl;
            case SDLK_LSHIFT:
            case SDLK_RSHIFT:
                return ImGuiKey_ModShift;
            case SDLK_LALT:
            case SDLK_RALT:
                return ImGuiKey_ModAlt;
            case SDLK_LGUI:
            case SDLK_RGUI:
                return ImGuiKey_ModSuper;
            default:
                return ImGuiKey_None;
        }
    }

    /**
     * @brief 向 ImGui 提交 SDL 当前四类修饰键状态
     *
     * @param io 接收修饰键事件的 ImGui IO
     */
    void updateImGuiModifiers(ImGuiIO &io)
    {
        const SDL_Keymod modifiers = SDL_GetModState();

        io.AddKeyEvent(ImGuiKey_ModCtrl, (modifiers & KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiKey_ModShift, (modifiers & KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiKey_ModAlt, (modifiers & KMOD_ALT) != 0);
        io.AddKeyEvent(ImGuiKey_ModSuper, (modifiers & KMOD_GUI) != 0);
    }

    /**
     * @brief 为 ImGui 提供 SDL 系统剪贴板读取回调
     *
     * @return 由静态缓冲区持有且在下次读取前有效的文本指针
     */
    const char *getClipboardText(void *)
    {
        static std::string clipboard;
        char *text = SDL_GetClipboardText();

        clipboard = text == nullptr ? std::string{} : text;
        SDL_free(text);

        return clipboard.c_str();
    }

    /**
     * @brief 为 ImGui 提供 SDL 系统剪贴板写入回调
     *
     * @param text 待写入的 UTF-8 文本，空指针会被转换为空字符串
     */
    void setClipboardText(void *, const char *text)
    {
        SDL_SetClipboardText(text == nullptr ? "" : text);
    }

    /**
     * @brief 将 SDL 鼠标、键盘、文本和窗口焦点事件送入 ImGui
     *
     * @param event 当前轮询到的 SDL 事件
     */
    void feedSdlEventToImGui(const SDL_Event &event)
    {
        ImGuiIO &io = ImGui::GetIO();
        switch (event.type)
        {
            case SDL_MOUSEMOTION:
                io.AddMousePosEvent(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            {
                int button = -1;
                if (event.button.button == SDL_BUTTON_LEFT)
                {
                    button = 0;
                }
                else if (event.button.button == SDL_BUTTON_RIGHT)
                {
                    button = 1;
                }
                else if (event.button.button == SDL_BUTTON_MIDDLE)
                {
                    button = 2;
                }

                if (button >= 0)
                {
                    io.AddMouseButtonEvent(button, event.type == SDL_MOUSEBUTTONDOWN);
                }

                break;
            }
            case SDL_MOUSEWHEEL:
            {
                const float direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0F : 1.0F;
                io.AddMouseWheelEvent(direction * static_cast<float>(event.wheel.x),
                    direction * static_cast<float>(event.wheel.y));

                break;
            }
            case SDL_TEXTINPUT:
                io.AddInputCharactersUTF8(event.text.text);
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
            {
                updateImGuiModifiers(io);
                if (const ImGuiKey key = keycodeToImGuiKey(event.key.keysym.sym); key != ImGuiKey_None)
                {
                    io.AddKeyEvent(key, event.type == SDL_KEYDOWN);
                }

                break;
            }
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
                {
                    io.AddFocusEvent(true);
                }
                else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                {
                    io.AddFocusEvent(false);
                }
                else if (event.window.event == SDL_WINDOWEVENT_LEAVE)
                {
                    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                }

                break;
            default:
                break;
        }
    }
}

UiContext::Impl::~Impl()
{
    shutdown();
}

void UiContext::Impl::init(const char *title, AppState &state)
{
    if (window_ != nullptr)
    {
        throw std::logic_error("UiContext is already initialized");
    }

    // 在创建平台资源前恢复配置，避免初始化后再调整窗口和字体
    UiConfig config;
    loadConfig(config);

    state.controlRatio = config.controlRatio;
    copyEndpointToInput(state);
    uiFontSize_ = config.uiFontSize;

    // 视频和事件子系统是窗口、OpenGL 上下文及文本输入的共同前置条件
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
    {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    window_ = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        config.windowWidth,
        config.windowHeight,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if (window_ == nullptr)
    {
        SDL_Quit();

        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }

    SDL_SetWindowMinimumSize(window_, kMinWindowWidth, kMinWindowHeight);

    // 将新上下文绑定到当前线程，后续所有 OpenGL 资源操作均在此线程执行
    glContext_ = SDL_GL_CreateContext(window_);
    if (glContext_ == nullptr)
    {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_Quit();

        throw std::runtime_error(std::string("SDL_GL_CreateContext failed: ") + SDL_GetError());
    }

    if (SDL_GL_MakeCurrent(window_, glContext_) != 0)
    {
        throw std::runtime_error(std::string("SDL_GL_MakeCurrent failed: ") + SDL_GetError());
    }

    vsyncEnabled_ = SDL_GL_SetSwapInterval(1) == 0;
    if (!vsyncEnabled_)
    {
        std::cerr << "VSync unavailable, enabling 60 FPS fallback limiter: " << SDL_GetError()
                << '\n';
    }

    int drawableWidth = 0;
    int drawableHeight = 0;

    SDL_GL_GetDrawableSize(window_, &drawableWidth, &drawableHeight);
    glViewport(0, 0, drawableWidth, drawableHeight);

    // ImGui 初始化顺序保证 IO、字体和样式在 OpenGL 后端创建前就绪
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.GetClipboardTextFn = getClipboardText;
    io.SetClipboardTextFn = setClipboardText;

    SDL_StartTextInput();
    loadFonts(uiFontSize_);
    loadedFontSize_ = uiFontSize_;

    ImGui::StyleColorsDark();

    ImGuiStyle &style = ImGui::GetStyle();
    style.ScaleAllSizes(kUiScale);
    style.WindowRounding = 4.0F;
    style.FrameRounding = 3.0F;
    style.ScrollbarSize = 20.0F;
    style.ItemSpacing.y = 6.0F;
    style.FramePadding.y = 5.0F;

    if (!ImGui_ImplOpenGL3_Init("#version 150"))
    {
        throw std::runtime_error("ImGui OpenGL backend initialization failed");
    }

    controlRatio_ = state.controlRatio;
    lastRenderTime_ = std::chrono::steady_clock::now();
    keyboardListening_ = keyboard_.startListening();

    std::cout << "Renderer: SDL2 + OpenGL + ImGui. Terminal KeyboardListener: "
            << (keyboardListening_ ? "enabled" : "disabled, SDL window keyboard still works")
            << '\n';
}

void UiContext::Impl::shutdown()
{
    if (window_ != nullptr)
    {
        // 只有活动窗口能够提供退出时应持久化的最终尺寸
        saveSettings();
    }

    keyboard_.stopListening();
    keyboardListening_ = false;

    SDL_StopTextInput();

    // 重新绑定所属上下文后释放纹理，避免在无当前上下文时调用 OpenGL
    if (window_ != nullptr && glContext_ != nullptr)
    {
        SDL_GL_MakeCurrent(window_, glContext_);
        destroyTexture(frameTex_);
        destroyTexture(histogramTex_);
    }

    // 渲染后端持有 ImGui 和字体纹理引用，必须先于 ImGui 上下文关闭
    if (ImGui::GetCurrentContext() != nullptr)
    {
        if (ImGui::GetIO().BackendRendererUserData != nullptr)
        {
            ImGui_ImplOpenGL3_Shutdown();
        }

        ImGui::DestroyContext();
    }

    if (glContext_ != nullptr)
    {
        SDL_GL_DeleteContext(glContext_);
        glContext_ = nullptr;
    }

    if (window_ != nullptr)
    {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }

    SDL_Quit();
}

void UiContext::Impl::loadFonts(const float size)
{
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();

    // 主字体缺失时退回内置字体，使非中文界面仍可使用
    ImFontConfig monoConfig;
    monoConfig.OversampleH = 2;
    monoConfig.OversampleV = 2;
    ImFont *font = io.Fonts->AddFontFromFileTTF(kJetBrainsMonoPath, size, &monoConfig);
    if (font == nullptr)
    {
        font = io.Fonts->AddFontDefault();
    }

    // 中文字形以 MergeMode 加入同一图集，调用方无需切换字体对象
    ImFontConfig cjkConfig;
    cjkConfig.MergeMode = true;
    cjkConfig.FontNo = 0;
    cjkConfig.OversampleH = 2;
    cjkConfig.OversampleV = 2;
    io.Fonts->AddFontFromFileTTF(kNotoSansCjkPath,
        size,
        &cjkConfig,
        io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    io.FontDefault = font;
}

void UiContext::Impl::setFontSize(const float size)
{
    const float nextSize = std::clamp(size, kMinUiFontSize, kMaxUiFontSize);
    if (std::abs(nextSize - uiFontSize_) < 0.01F)
    {
        return;
    }

    uiFontSize_ = nextSize;

    // 连续字号调整先用全局比例即时反馈，再在输入稳定后重建清晰字体
    if (ImGui::GetCurrentContext() != nullptr)
    {
        fontRebuildPending_ = true;
        fontRebuildAt_ = std::chrono::steady_clock::now() + kFontRebuildDelay;
    }
}

void UiContext::Impl::rebuildFontsIfDue()
{
    if (!fontRebuildPending_ || std::chrono::steady_clock::now() < fontRebuildAt_)
    {
        return;
    }

    loadFonts(uiFontSize_);

    // 清空并重载字体后同步替换 OpenGL 字体纹理
    if (ImGui::GetIO().BackendRendererUserData != nullptr)
    {
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        if (!ImGui_ImplOpenGL3_CreateFontsTexture())
        {
            throw std::runtime_error("failed to rebuild ImGui font texture");
        }
    }

    loadedFontSize_ = uiFontSize_;
    fontRebuildPending_ = false;
}

void UiContext::Impl::getWindowSize(int &width, int &height) const
{
    SDL_GetWindowSize(window_, &width, &height);
}

void UiContext::Impl::processEvents(AppState &state)
{
    // SDL 事件统一交给单事件处理器，以保持 ImGui 与应用快捷键顺序一致
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0)
    {
        processSdlEvent(event, state);
    }

    // 终端事件绕过 SDL 文本输入状态，作为额外快捷键来源处理
    if (keyboardListening_)
    {
        KeyboardListener::KeyEvent keyEvent;
        while (keyboard_.popNextEvent(keyEvent))
        {
            handleKey(static_cast<int>(keyEvent.keyCode & 0xFFFFu), state);
        }
    }
}

void UiContext::Impl::processSdlEvent(const SDL_Event &event, AppState &state)
{
    // Ctrl+Shift+滚轮属于全局字号命令，消费后禁止控件同时滚动
    if (event.type == SDL_MOUSEWHEEL && (SDL_GetModState() & KMOD_CTRL) != 0 &&
        (SDL_GetModState() & KMOD_SHIFT) != 0)
    {
        const float delta = static_cast<float>(event.wheel.y) * 2.0F;
        setFontSize(uiFontSize_ + delta);

        return;
    }

    feedSdlEventToImGui(event);

    if (event.type == SDL_QUIT)
    {
        state.quit = true;
    }

    if (ImGui::GetIO().WantTextInput)
    {
        return;
    }
    else if (event.type == SDL_KEYDOWN && event.key.repeat == 0)
    {
        if (const SDL_Keycode sym = event.key.keysym.sym; sym >= 0 && sym <= 127)
        {
            handleKey(sym, state);
        }
        else if (sym == SDLK_ESCAPE)
        {
            handleKey(27, state);
        }
    }
}

void UiContext::Impl::beginFrame(const float deltaTime)
{
    rebuildFontsIfDue();

    int width = 0;
    int height = 0;

    SDL_GL_GetDrawableSize(window_, &width, &height);
    glViewport(0, 0, width, height);

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DeltaTime = deltaTime;
    io.FontGlobalScale = uiFontSize_ / loadedFontSize_;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
}

void UiContext::Impl::endFrame()
{
    ImGui::Render();
    ImDrawData *drawData = ImGui::GetDrawData();
    const int framebufferWidth =
            static_cast<int>(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const int framebufferHeight =
            static_cast<int>(drawData->DisplaySize.y * drawData->FramebufferScale.y);

    // 帧缓冲区无有效尺寸或窗口最小化时跳过交换并主动休眠
    if (framebufferWidth <= 0 || framebufferHeight <= 0 ||
        (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0)
    {
        std::this_thread::sleep_for(kMinimizedFrameInterval);
        nextFrameAt_ = {};

        return;
    }

    glViewport(0, 0, framebufferWidth, framebufferHeight);
    glClearColor(0.055F, 0.058F, 0.065F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(drawData);
    SDL_GL_SwapWindow(window_);

    // 软件限帧基于上次计划时刻递增，避免每帧耗时累积为长期漂移
    if (!vsyncEnabled_)
    {
        if (const auto now = std::chrono::steady_clock::now(); nextFrameAt_.time_since_epoch().count() == 0 ||
            nextFrameAt_ <= now)
        {
            nextFrameAt_ = now + kFallbackFrameInterval;
        }
        else
        {
            nextFrameAt_ += kFallbackFrameInterval;
        }

        std::this_thread::sleep_until(nextFrameAt_);
    }
}

void UiContext::Impl::uploadTexture(Texture &texture, const cv::Mat &bgr)
{
    if (bgr.empty())
    {
        return;
    }

    if (bgr.type() != CV_8UC3)
    {
        throw std::invalid_argument("texture upload requires a CV_8UC3 image");
    }

    if (texture.id == 0)
    {
        glGenTextures(1, &texture.id);
        glBindTexture(GL_TEXTURE_2D, texture.id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    else
    {
        glBindTexture(GL_TEXTURE_2D, texture.id);
    }

    // 非连续 Mat 通过 GL_UNPACK_ROW_LENGTH 保留其实际行跨度
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,
        bgr.isContinuous() ? 0 : static_cast<GLint>(bgr.step / bgr.elemSize()));

    if (texture.width != bgr.cols || texture.height != bgr.rows)
    {
        glTexImage2D(GL_TEXTURE_2D,
            0,
            GL_RGB,
            bgr.cols,
            bgr.rows,
            0,
            GL_BGR,
            GL_UNSIGNED_BYTE,
            bgr.data);
        texture.width = bgr.cols;
        texture.height = bgr.rows;
    }
    else
    {
        glTexSubImage2D(GL_TEXTURE_2D,
            0,
            0,
            0,
            bgr.cols,
            bgr.rows,
            GL_BGR,
            GL_UNSIGNED_BYTE,
            bgr.data);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void UiContext::Impl::destroyTexture(Texture &texture)
{
    if (texture.id != 0)
    {
        glDeleteTextures(1, &texture.id);
        texture = {};
    }
}

void UiContext::Impl::uploadFrameTexture(const cv::Mat &frameBgr)
{
    uploadTexture(frameTex_, frameBgr);
}

void UiContext::Impl::uploadHistogramTexture(const cv::Mat &histBgr)
{
    uploadTexture(histogramTex_, histBgr);
}

void UiContext::Impl::clearFrameTexture()
{
    destroyTexture(frameTex_);
}

void UiContext::Impl::clearHistogramTexture()
{
    destroyTexture(histogramTex_);
}

ImVec2 UiContext::Impl::scaledImageSize(const cv::Mat &image, const ImVec2 &available,
    const AppState &state) const
{
    if (image.empty())
    {
        return {640.0F, 480.0F};
    }

    const auto width = static_cast<float>(image.cols);
    const auto height = static_cast<float>(image.rows);
    if (!state.fitToWindow)
    {
        return {width * state.zoom, height * state.zoom};
    }

    const float sx = available.x > 1.0F ? available.x / width : 1.0F;
    const float sy = available.y > 1.0F ? available.y / height : 1.0F;
    const float scaleBase = std::min(sx, sy);
    const float scale =
            std::clamp(scaleBase, static_cast<float>(kMinZoom), static_cast<float>(kMaxZoom));

    return {width * scale, height * scale};
}

void UiContext::Impl::ensureHistogramFrame(AppState &state)
{
    if (state.lastHistogramFrame.empty() && !state.lastFrame.empty())
    {
        state.lastHistogramFrame = makeHistogram(state.lastFrame, state.mode, &state);
    }
}

void UiContext::Impl::draw(AppState &state)
{
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    state.hoverSampleTimer += ImGui::GetIO().DeltaTime;
    drawImageWindow(state, displaySize);
    drawControlPanel(state, displaySize);
    drawControlSplitter(state, displaySize);
}

void UiContext::Impl::handleSaveRequests(AppState &state)
{
    if (state.saveRawRequested)
    {
        state.saveRawRequested = false;
        state.saveStatus = imageSaver_.submit(state.lastFrame, "raw_frame", "raw") ?
                "Saving raw frame..." :
                "No raw frame to save";
    }

    if (state.saveViewRequested)
    {
        state.saveViewRequested = false;
        const cv::Mat &view = state.lastViewFrame.empty() ? state.lastFrame : state.lastViewFrame;

        state.saveStatus = imageSaver_.submit(view, "view_frame", "view") ?
                "Saving current view..." :
                "No view frame to save";
    }

    if (state.saveHistogramRequested)
    {
        state.saveHistogramRequested = false;
        ensureHistogramFrame(state);

        state.saveStatus = imageSaver_.submit(state.lastHistogramFrame, "histogram", "histogram") ?
                "Saving histogram..." :
                "No histogram to save";
    }

    if (auto result = imageSaver_.pollResult())
    {
        state.saveStatus = std::move(*result);
    }
}

bool UiContext::Impl::updateFrameTexture(AppState &state)
{
    const bool imageDirty = !state.lastFrame.empty() &&
    (state.imageRevision != pf_.displayedRevision || state.mode != pf_.displayedMode ||
        state.analysisSettingsChanged);
    if (!imageDirty)
    {
        return false;
    }

    const bool displayedNewLiveFrame =
            !state.paused && state.imageRevision != pf_.displayedRevision;
    state.lastViewFrame = makeDisplayImage(state.lastFrame, state.mode);

    if (state.showExposureWarnings)
    {
        applyExposureWarnings(state.lastViewFrame,
            state.lastFrame,
            state.shadowThreshold,
            state.highlightThreshold);
    }

    uploadFrameTexture(state.lastViewFrame);
    pf_.displayedRevision = state.imageRevision;
    pf_.displayedMode = state.mode;

    return displayedNewLiveFrame;
}

void UiContext::Impl::updateHistogramTexture(AppState &state, const bool analysisDirty)
{
    const bool histogramFrameChanged =
            state.imageRevision != pf_.histogramRevision || state.mode != pf_.histogramMode;
    const bool histogramUpdateDue = pf_.lastHistogramUpdate.time_since_epoch().count() == 0 ||
            std::chrono::steady_clock::now() - pf_.lastHistogramUpdate >=
            std::chrono::milliseconds(100);

    // 用户请求和参数变化不节流，只有连续到达的新帧受更新时间限制
    if (state.histogramMode != HistogramMode::Off && !state.lastFrame.empty() &&
        (state.histogramRefreshRequested || !pf_.histogramTextureReady || analysisDirty ||
            (histogramFrameChanged && histogramUpdateDue)))
    {
        const cv::Rect roi = selectedRoi(state.lastFrame, state);
        const cv::Mat histogramSource =
                state.roiValid && !roi.empty() ? state.lastFrame(roi) : state.lastFrame;

        state.lastHistogramFrame = makeHistogram(histogramSource, state.mode, &state);

        if (!state.lastHistogramFrame.empty())
        {
            uploadHistogramTexture(state.lastHistogramFrame);
            pf_.histogramTextureReady = true;
            pf_.histogramRevision = state.imageRevision;
            pf_.histogramMode = state.mode;
            pf_.lastHistogramUpdate = std::chrono::steady_clock::now();
        }
    }
    else if (state.histogramMode == HistogramMode::Off && pf_.histogramTextureReady)
    {
        clearHistogramTexture();
        state.histogramHoverValid = false;
        pf_.histogramTextureReady = false;
        pf_.lastHistogramUpdate = {};
    }
}

bool UiContext::Impl::updateAndRender(AppState &state)
{
    if (state.endpointInputResetRequested)
    {
        copyEndpointToInput(state);
        state.endpointInputResetRequested = false;
    }

    const auto now = std::chrono::steady_clock::now();
    const float deltaTime = std::clamp(
        std::chrono::duration<float>(now - lastRenderTime_).count(), 1.0F / 120.0F, 0.1F);
    lastRenderTime_ = now;

    handleSaveRequests(state);

    const bool analysisDirty = state.roiChanged || state.analysisSettingsChanged;
    const bool displayedNewLiveFrame = updateFrameTexture(state);
    updateHistogramTexture(state, analysisDirty);

    // 派生纹理均观察到变更后再清除一次性标志，防止遗漏刷新
    state.histogramRefreshRequested = false;
    state.roiChanged = false;
    state.analysisSettingsChanged = false;

    beginFrame(deltaTime);
    draw(state);
    endFrame();

    controlRatio_ = state.controlRatio;

    return displayedNewLiveFrame;
}

void UiContext::Impl::onEndpointChanged(AppState &state)
{
    // 端点变化使所有源相关 GPU 资源、派生图像和版本记录失效
    copyEndpointToInput(state);
    clearFrameTexture();
    clearHistogramTexture();

    state.hoverPixelValid = false;
    state.histogramHoverValid = false;
    state.lastViewFrame.release();
    state.lastHistogramFrame.release();

    pf_.histogramTextureReady = false;
    pf_.histogramRevision = state.imageRevision;
    pf_.histogramMode = state.mode;
    pf_.lastHistogramUpdate = {};
    pf_.displayedRevision = 0;
}

void UiContext::Impl::saveSettings() const
{
    UiConfig config;
    config.controlRatio = controlRatio_;
    config.uiFontSize = uiFontSize_;
    getWindowSize(config.windowWidth, config.windowHeight);

    if (!saveConfig(config))
    {
        std::cerr << "Failed to save UI settings\n";
    }
}

UiContext::UiContext(): impl_(std::make_unique<Impl>())
{
}

UiContext::~UiContext() = default;

UiContext::UiContext(UiContext &&) noexcept = default;

UiContext &UiContext::operator=(UiContext &&) noexcept = default;

void UiContext::init(const char *title, AppState &state) const
{
    impl_->init(title, state);
}

void UiContext::processEvents(AppState &state) const
{
    impl_->processEvents(state);
}

bool UiContext::updateAndRender(AppState &state) const
{
    return impl_->updateAndRender(state);
}

void UiContext::onEndpointChanged(AppState &state) const
{
    impl_->onEndpointChanged(state);
}

namespace
{
    void copyEndpointToInput(AppState &state)
    {
        std::snprintf(state.endpointInput.data(), state.endpointInput.size(), "%s",
            state.endpoint.c_str());
    }
}
