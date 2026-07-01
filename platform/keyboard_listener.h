#ifndef KEYBOARD_LISTENER_H_
#define KEYBOARD_LISTENER_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <termios.h>
#include <thread>
#include <vector>

/**
 * @brief 从终端采集按键并生成点击或长按事件
 *
 * @details 监听器临时接管标准输入的终端模式，并使用独立线程完成字节采集和
 * 事件判定。事件通过线程安全队列交由调用线程消费
 */
class KeyboardListener
{
public:

    /**
     * @brief 按键持续时间对应的事件类别
     */
    enum class EventType
    {
        Click, ///< 在点击释放窗口内结束的按键
        LongPress ///< 持续时间达到长按阈值的按键
    };

    /**
     * @brief 一次已判定的终端按键事件
     */
    struct KeyEvent
    {
        // === 按键语义 ===
        EventType type{}; ///< 按键的点击或长按判定结果
        bool ctrl{false}; ///< 事件是否携带 Ctrl 修饰位
        bool alt{false}; ///< 事件是否携带 Alt 修饰位
        bool shift{false}; ///< 事件是否携带 Shift 修饰位
        uint32_t keyCode{0}; ///< 修饰位与基础键值合成后的键码

        // === 原始输入与时序 ===
        std::vector<uint8_t> rawBytes; ///< 终端读取到的原始转义序列
        std::chrono::steady_clock::time_point ts{}; ///< 事件完成判定的单调时钟时刻
    };

    /**
     * @brief 创建尚未启动的终端键盘监听器
     *
     * @param longPressMs 判定为长按所需的持续时间，单位为毫秒
     * @param repeatGapMs 长按后等待后续重复输入的时间，单位为毫秒
     * @param releaseGapMs 单击判定等待窗口，单位为毫秒
     */
    KeyboardListener(int longPressMs = 300, int repeatGapMs = 90, int releaseGapMs = 120);

    /**
     * @brief 停止监听线程并恢复标准输入原始状态
     */
    ~KeyboardListener();

    /**
     * @brief 切换终端模式并启动采集与事件判定线程
     *
     * @return 成功启动或已经处于监听状态时返回 true，终端初始化失败时返回 false
     */
    bool startListening();

    /**
     * @brief 请求监听线程退出并恢复终端属性
     */
    void stopListening();

    /**
     * @brief 查询监听线程是否处于运行状态
     *
     * @return 已启动且尚未停止时返回 true
     */
    [[nodiscard]] bool isListening() const;

    /**
     * @brief 从事件队列中非阻塞取出最早事件
     *
     * @param event 成功时接收队首事件
     *
     * @return 取到事件时返回 true，队列为空时返回 false
     */
    bool popNextEvent(KeyEvent &event);

private:

    /**
     * @brief 原始终端字节解码后的键值与修饰信息
     */
    struct DecodedKey
    {
        bool ctrl{false}; ///< 解码序列是否携带 Ctrl 修饰位
        bool alt{false}; ///< 解码序列是否携带 Alt 修饰位
        bool shift{false}; ///< 解码序列是否携带 Shift 修饰位
        uint16_t baseCode{0}; ///< 不包含修饰位的基础或扩展键值
        std::vector<uint8_t> raw; ///< 产生该结果的完整终端字节序列
    };

    /**
     * @brief 尚未完成释放判定的按键状态
     */
    struct PressState
    {
        // === 按压时序 ===
        std::chrono::steady_clock::time_point firstTs{}; ///< 当前按键序列首次出现的时刻
        std::chrono::steady_clock::time_point lastTs{}; ///< 最近收到重复输入的时刻
        bool longReported{false}; ///< 当前按键是否已经产生长按事件

        // === 最近输入 ===
        std::vector<uint8_t> lastRaw; ///< 最近一次采集到的原始按键序列
        bool ctrl{false}; ///< 最近输入携带的 Ctrl 状态
        bool alt{false}; ///< 最近输入携带的 Alt 状态
        bool shift{false}; ///< 最近输入携带的 Shift 状态
    };

    /**
     * @brief 将基础键值和修饰键编码到统一键码
     */
    static uint32_t composeKeyCode(bool ctrl, bool alt, bool shift, uint16_t baseCode);

    /**
     * @brief 判断字节是否位于 ASCII 数字区间
     */
    static bool isAsciiDigit(uint8_t ch);

    /**
     * @brief 保存终端状态并启用非阻塞原始输入模式
     */
    bool enableRawMode();

    /**
     * @brief 恢复监听前保存的终端属性和文件标志
     */
    void disableRawMode();

    /**
     * @brief 解析普通字符、控制字符及常见终端转义序列
     */
    static std::optional<DecodedKey> decodeInputBytes(const std::vector<uint8_t> &rawBytes);

    /** @brief 将已判定事件追加到主线程消费队列 */
    void pushEvent(const KeyEvent &event);

    /**
     * @brief 持续读取标准输入并更新活跃按键状态
     */
    void captureLoop();

    /**
     * @brief 根据时间阈值生成点击或长按事件
     */
    void eventDetectionLoop();

    // === 判定阈值 ===
    int longPressThresholdMs_; ///< 首次输入到长按成立的毫秒数
    int longPressReleaseGapMs_; ///< 长按后无重复输入即视为释放的毫秒数
    int clickReleaseGapMs_; ///< 短按后无重复输入即视为释放的毫秒数

    // === 监听生命周期 ===
    std::atomic<bool> listening_{false}; ///< 两个后台循环共享的运行开关
    std::thread captureThread_; ///< 标准输入字节采集线程
    std::thread eventThread_; ///< 点击和长按状态判定线程

    // === 按键状态 ===
    std::mutex stateMutex_; ///< 保护活跃按键状态的并发访问
    std::map<uint32_t, PressState> activeKeyStates_; ///< 按合成键码索引的未释放按键

    // === 事件队列 ===
    std::mutex eventQueueMutex_; ///< 保护生产线程与消费线程之间的事件队列
    std::queue<KeyEvent> eventQueue_; ///< 等待调用线程取出的已完成事件

    // === 终端模式恢复 ===
    termios originalTermios_{}; ///< 启用原始模式前的终端属性快照
    int originalFileFlags_{-1}; ///< 启用非阻塞读取前的标准输入文件标志
    bool rawModeEnabled_{false}; ///< 是否持有需要恢复的终端状态
};

#endif
