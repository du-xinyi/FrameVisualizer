#include "keyboard_listener.h"

#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <system_error>
#include <unistd.h>

KeyboardListener::KeyboardListener(const int longPressMs, const int repeatGapMs, const int releaseGapMs)
    : longPressThresholdMs_(longPressMs), longPressReleaseGapMs_(repeatGapMs), clickReleaseGapMs_(releaseGapMs)
{
}

KeyboardListener::~KeyboardListener()
{
    stopListening();
}

bool KeyboardListener::startListening()
{
    if (listening_.exchange(true))
    {
        return true;
    }

    if (!enableRawMode())
    {
        listening_ = false;

        return false;
    }

    try
    {
        captureThread_ = std::thread(&KeyboardListener::captureLoop, this);
        eventThread_ = std::thread(&KeyboardListener::eventDetectionLoop, this);
    }
    catch (const std::system_error &error)
    {
        listening_ = false;
        if (captureThread_.joinable())
        {
            captureThread_.join();
        }
        disableRawMode();
        std::cerr << "Failed to start keyboard listener: " << error.what() << '\n';
        return false;
    }

    return true;
}

void KeyboardListener::stopListening()
{
    listening_ = false;

    if (captureThread_.joinable())
    {
        captureThread_.join();
    }

    if (eventThread_.joinable())
    {
        eventThread_.join();
    }

    disableRawMode();
}

bool KeyboardListener::isListening() const
{
    return listening_.load();
}

bool KeyboardListener::popNextEvent(KeyEvent &event)
{
    std::lock_guard lk(eventQueueMutex_);
    if (eventQueue_.empty())
    {
        return false;
    }

    event = std::move(eventQueue_.front());
    eventQueue_.pop();
    return true;
}

uint32_t KeyboardListener::composeKeyCode(const bool ctrl, const bool alt, const bool shift, const uint16_t baseCode)
{
    uint32_t keyCode = baseCode;

    if (ctrl)
    {
        keyCode |= (1u << 23);
    }

    if (alt)
    {
        keyCode |= (1u << 22);
    }

    if (shift)
    {
        keyCode |= (1u << 21);
    }

    return keyCode;
}

bool KeyboardListener::enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &originalTermios_) == -1)
    {
        std::perror("tcgetattr");

        return false;
    }
    originalFileFlags_ = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (originalFileFlags_ == -1)
    {
        std::perror("fcntl(F_GETFL)");
        return false;
    }

    termios raw = originalTermios_;
    // 禁用行缓冲、回显和信号转换，使控制键以原始字节进入解析器
    const auto localMask = static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_lflag &= static_cast<tcflag_t>(~localMask);

    const auto inputMask = static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_iflag &= static_cast<tcflag_t>(~inputMask);
    raw.c_cflag |= CS8;
    // 禁用终端输出转换，避免监听期间日志换行被隐式改写
    const auto outputMask = static_cast<tcflag_t>(OPOST);
    raw.c_oflag &= static_cast<tcflag_t>(~outputMask);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    {
        std::perror("tcsetattr");

        return false;
    }

    if (fcntl(STDIN_FILENO, F_SETFL, originalFileFlags_ | O_NONBLOCK) == -1)
    {
        std::perror("fcntl(F_SETFL)");
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &originalTermios_);
        originalFileFlags_ = -1;
        return false;
    }

    rawModeEnabled_ = true;

    return true;
}

void KeyboardListener::disableRawMode()
{
    if (!rawModeEnabled_)
    {
        return;
    }

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &originalTermios_);
    if (originalFileFlags_ != -1)
    {
        fcntl(STDIN_FILENO, F_SETFL, originalFileFlags_);
        originalFileFlags_ = -1;
    }

    rawModeEnabled_ = false;
}

bool KeyboardListener::isAsciiDigit(const uint8_t ch)
{
    return ch >= '0' && ch <= '9';
}

std::optional<KeyboardListener::DecodedKey> KeyboardListener::decodeInputBytes(const std::vector<uint8_t> &rawBytes)
{
    if (rawBytes.empty())
    {
        return std::nullopt;
    }

    DecodedKey decodedKey;
    decodedKey.raw = rawBytes;

    // 终端用 0x01 至 0x1A 表示 Ctrl 与字母的组合，需要还原基础字母
    if (rawBytes.size() == 1 && rawBytes[0] >= 1 && rawBytes[0] <= 26)
    {
        decodedKey.ctrl = true;
        decodedKey.baseCode = static_cast<uint16_t>('A' + rawBytes[0] - 1);

        return decodedKey;
    }

    // 单字节序列直接保留键值，大写字母同时携带 Shift 语义
    if (rawBytes.size() == 1)
    {
        const uint8_t b = rawBytes[0];
        if (b >= 'A' && b <= 'Z')
        {
            decodedKey.shift = true;
        }

        decodedKey.baseCode = b;

        return decodedKey;
    }

    // 常见终端将 Alt 组合编码为 ESC 前缀与基础键两个字节
    if (rawBytes.size() == 2 && rawBytes[0] == 0x1B)
    {
        decodedKey.alt = true;
        const uint8_t b = rawBytes[1];
        if (b >= 'A' && b <= 'Z')
        {
            decodedKey.shift = true;
        }

        decodedKey.baseCode = b;

        return decodedKey;
    }

    // 无修饰方向键使用三字节 CSI 序列并映射到内部扩展键值
    if (rawBytes.size() == 3 && rawBytes[0] == 0x1B && rawBytes[1] == '[')
    {
        switch (rawBytes[2])
        {
            case 'A': decodedKey.baseCode = 0x0101;
                return decodedKey;
            case 'B': decodedKey.baseCode = 0x0102;
                return decodedKey;
            case 'C': decodedKey.baseCode = 0x0103;
                return decodedKey;
            case 'D': decodedKey.baseCode = 0x0104;
                return decodedKey;
            default: break;
        }
    }

    // xterm 在 CSI 参数 m 中编码 Shift、Alt 和 Ctrl 的组合
    if (rawBytes.size() == 6 && rawBytes[0] == 0x1B && rawBytes[1] == '['
        && rawBytes[2] == '1' && rawBytes[3] == ';' && isAsciiDigit(rawBytes[4]))
    {
        const int modifierCode = rawBytes[4] - '0';
        decodedKey.shift = (modifierCode == 2 || modifierCode == 4 || modifierCode == 6 || modifierCode == 8);
        decodedKey.alt = (modifierCode == 3 || modifierCode == 4 || modifierCode == 7 || modifierCode == 8);
        decodedKey.ctrl = (modifierCode == 5 || modifierCode == 6 || modifierCode == 7 || modifierCode == 8);
        switch (rawBytes[5])
        {
            case 'A': decodedKey.baseCode = 0x0101;
                return decodedKey;
            case 'B': decodedKey.baseCode = 0x0102;
                return decodedKey;
            case 'C': decodedKey.baseCode = 0x0103;
                return decodedKey;
            case 'D': decodedKey.baseCode = 0x0104;
                return decodedKey;
            default: break;
        }
    }

    // 未识别序列退化为首字节事件，保持输入可观察而非静默丢弃
    decodedKey.baseCode = rawBytes[0];

    return decodedKey;
}

void KeyboardListener::pushEvent(const KeyEvent &event)
{
    std::lock_guard lk(eventQueueMutex_);
    eventQueue_.push(event);
}

void KeyboardListener::captureLoop()
{
    while (listening_)
    {
        uint8_t buf[16];
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));

        if (n <= 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

            continue;
        }

        std::vector rawBytes(buf, buf + n);
        // 独立 ESC 既作为一次点击交付，也用于终止终端监听循环
        if (rawBytes.size() == 1 && rawBytes[0] == 0x1B)
        {
            KeyEvent escEvent;
            escEvent.type = EventType::Click;
            escEvent.ctrl = false;
            escEvent.alt = false;
            escEvent.shift = false;
            escEvent.keyCode = composeKeyCode(false, false, false, 0x1B);
            escEvent.rawBytes = rawBytes;
            escEvent.ts = std::chrono::steady_clock::now();
            pushEvent(escEvent);
            listening_ = false;

            break;
        }

        auto decodedKey = decodeInputBytes(rawBytes);
        if (!decodedKey.has_value())
        {
            continue;
        }

        const uint32_t compositeKeyCode = composeKeyCode(decodedKey->ctrl, decodedKey->alt, decodedKey->shift,
            decodedKey->baseCode);
        const auto nowTs = std::chrono::steady_clock::now();

        std::lock_guard lk(stateMutex_);
        auto &[firstPressTs, lastInputTs, longPressReported, lastRawBytes, hasCtrl, hasAlt, hasShift] = activeKeyStates_
                [compositeKeyCode];
        // 首次输入建立按下起点，终端自动重复输入只刷新最近活动时刻
        if (firstPressTs.time_since_epoch().count() == 0)
        {
            firstPressTs = nowTs;
            longPressReported = false;
        }

        lastInputTs = nowTs;
        hasCtrl = decodedKey->ctrl;
        hasAlt = decodedKey->alt;
        hasShift = decodedKey->shift;
        lastRawBytes = decodedKey->raw;
    }
}

void KeyboardListener::eventDetectionLoop()
{
    using namespace std::chrono;
    while (listening_)
    {
        std::vector<KeyEvent> pendingEvents;
        const auto nowTs = steady_clock::now();

        {
            std::lock_guard lk(stateMutex_);
            for (auto it = activeKeyStates_.begin(); it != activeKeyStates_.end();)
            {
                auto &[firstPressTs, lastInputTs, longPressReported, lastRawBytes, hasCtrl, hasAlt, hasShift] = it->
                        second;
                const auto heldMs = duration_cast<milliseconds>(nowTs - firstPressTs).count();
                const auto idleMs = duration_cast<milliseconds>(nowTs - lastInputTs).count();

                // 同一次持续按键达到阈值后仅生成一个长按事件
                if (!longPressReported && heldMs >= longPressThresholdMs_)
                {
                    KeyEvent longPressEvent;
                    longPressEvent.type = EventType::LongPress;
                    longPressEvent.keyCode = it->first;
                    longPressEvent.ctrl = hasCtrl;
                    longPressEvent.alt = hasAlt;
                    longPressEvent.shift = hasShift;
                    longPressEvent.rawBytes = lastRawBytes;
                    longPressEvent.ts = nowTs;
                    pendingEvents.push_back(std::move(longPressEvent));
                    longPressReported = true;
                }

                // 短按空闲后生成点击，长按上报后仅等待空闲窗口清理状态
                const bool canReleaseAsClick = !longPressReported && idleMs >= clickReleaseGapMs_ && heldMs <
                        longPressThresholdMs_;

                if (const bool canReleaseAfterLong = longPressReported && idleMs >= longPressReleaseGapMs_;
                    canReleaseAsClick || canReleaseAfterLong)
                {
                    if (!longPressReported)
                    {
                        KeyEvent clickEvent;
                        clickEvent.type = EventType::Click;
                        clickEvent.keyCode = it->first;
                        clickEvent.ctrl = hasCtrl;
                        clickEvent.alt = hasAlt;
                        clickEvent.shift = hasShift;
                        clickEvent.rawBytes = lastRawBytes;
                        clickEvent.ts = nowTs;
                        pendingEvents.push_back(std::move(clickEvent));
                    }
                    it = activeKeyStates_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (const auto &event: pendingEvents)
        {
            pushEvent(event);
        }

        std::this_thread::sleep_for(milliseconds(2));
    }
}
