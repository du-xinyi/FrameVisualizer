# frame-scope

frame-scope 是一个面向 OpenCV 图像流的 Linux 桌面查看与分析工具。它既能播放本地视频，
也能通过 ZeroMQ 接收实时 `cv::Mat`，适合调试摄像头、视觉算法输出和多路图像管线。

项目提供 SDL2、OpenGL 与 Dear ImGui 驱动的桌面界面，同时包含可嵌入其他 CMake
工程的 `FrameSender` 库、接收端库和合成帧发布器。

## 主要能力

- 本地视频：拖放打开、按帧定位、暂停、逐帧查看和 `0.25x`～`4x` 倍速播放
- 实时图像流：接收 `topic + FrameMeta + payload` 三段 ZeroMQ 消息，支持 JPEG 和 RAW
- 多摄像头：自动发现 `source_id`，可自动锁定、手动选择或混合查看全部来源
- 低延迟处理：后台解码、最新帧优先，并在复制 payload 前过滤未选中的摄像头
- 图像分析：RGB、灰度、HSV、单通道视图，像素取样、ROI 统计和曝光检查
- 直方图：线性、对数、累计模式，并可查看各 bin 的像素数量
- 帧保存：异步保存原始帧、当前视图或直方图 PNG
- 运行统计：源帧率、接收帧率、渲染帧率、跳帧、负载大小和链路延迟
- 暂停回看：保留最近 60 帧，并以 256 MiB 为历史缓存上限

## 快速开始

### 环境要求

- Linux
- 支持 C++20 的 GCC 或 Clang
- CMake 3.28+
- OpenCV 4
- ZeroMQ（libzmq）
- protobuf 库与 `protoc`
- SDL2、OpenGL 和 pkg-config

Ubuntu/Debian 可安装基础依赖：

```bash
sudo apt update
sudo apt install build-essential cmake pkg-config \
  libopencv-dev libzmq3-dev \
  libprotobuf-dev protobuf-compiler \
  libsdl2-dev libgl1-mesa-dev
```

Dear ImGui 源码已包含在 `third_party/imgui`，无需单独安装。若发行版提供的 CMake
低于 3.28，请使用 CMake 官方发行包或其他可信的软件源升级。

### 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

默认生成以下目标：

| 目标 | 用途 |
| --- | --- |
| `frame-scope` | 桌面查看与分析应用 |
| `SyntheticFramePublisher` | 生成 1280×720、30 FPS 的合成 JPEG 流 |
| `FrameSender` | 将 `cv::Mat` 发布到 ZeroMQ 的动态库 |
| `FrameReceiver` | 接收并筛选最新帧的动态库 |
| `FrameProtocol` | protobuf 协议动态库 |

### 运行合成示例

先启动发布器：

```bash
./build/SyntheticFramePublisher
```

再打开另一个终端运行查看器：

```bash
./build/frame-scope
```

发布器默认绑定 `tcp://*:5555`，查看器默认连接 `tcp://127.0.0.1:5555`。也可以通过
首个命令行参数指定端点：

```bash
./build/SyntheticFramePublisher tcp://*:6000
./build/frame-scope tcp://127.0.0.1:6000
```

ZeroMQ PUB/SUB 不会补发订阅建立前的消息，因此启动时丢失少量帧属于正常现象。

## 使用方式

### 查看本地视频

启动 `frame-scope` 后，将 OpenCV `VideoCapture` 支持的视频文件拖入窗口即可播放。
视频模式下，底部时间轴显示当前位置、总时长和倍速；右上角 `X` 用于关闭视频并返回
实时图像流。

| 操作 | 功能 |
| --- | --- |
| 拖动时间轴 | 按帧定位 |
| `Space` | 暂停或继续 |
| `Left` / `Right` | 后退或前进 1 帧 |
| 长按方向键 0.5～1.5 秒 | 每次键盘重复移动 5 帧 |
| 长按方向键超过 1.5 秒 | 每次键盘重复移动 10 帧 |
| `Shift + Left` / `Shift + Right` | 后退或前进 5 帧 |
| 倍速菜单 | 选择 `0.25x`～`4x` 播放速度 |
| 右上角 `X` | 关闭视频并返回 ZeroMQ 输入 |

实际支持的容器和编码格式取决于当前 OpenCV 视频后端。

### 查看实时图像流

实时模式的 **Connection** 区域用于查看连接状态、修改端点和选择摄像头。输入新的
ZeroMQ 端点后按 Enter 或点击 **Apply EP** 即可切换；切换失败时保留原连接。

接收链路采用最新帧优先策略：

```text
ZeroMQ SUB
    └─ 读取 topic 与 FrameMeta
        ├─ 未选中的 source_id：排空 payload，不复制、不解码
        └─ 选中的 source_id：复制最新 payload → 后台解码 → 显示与分析
```

该策略避免其他摄像头占用不必要的内存带宽和 JPEG/RAW 解码时间。接收队列默认最多
保留 64 条消息，用于吸收多路摄像头在两次 UI 轮询之间形成的短时突发。

### 选择摄像头

多个摄像头共用一个端点时，每个发布端都必须设置唯一且稳定的
`FrameSenderOptions::sourceId`。**Camera** 下拉框会列出运行期间发现的来源：

| 模式 | 行为 |
| --- | --- |
| **Auto (first available)** | 默认模式，锁定首个收到的有效 `source_id` |
| 具体 `source_id` | 只复制和解码所选摄像头的帧 |
| **All cameras (mixed)** | 关闭来源过滤，不同摄像头画面可能交替显示 |

切换摄像头会清除上一来源的画面、暂停历史、ROI 和统计数据，避免旧来源内容短暂残留。
界面同时显示已发现摄像头数量、累计过滤帧数和当前活动 `source_id`。

### 图像查看与分析

| 操作 | 功能 |
| --- | --- |
| `1` / `2` / `3` | RGB / 灰度 / HSV 视图 |
| `4` / `5` / `6` | R/G/B 或 H/S/V 单通道视图 |
| `H` | 开关直方图 |
| `F` | 开关自适应窗口 |
| `+` / `-` | 放大或缩小 |
| `Ctrl + 滚轮` | 以鼠标位置为中心缩放 |
| 鼠标左键拖动 | 平移手动缩放后的图像 |
| `Shift + 鼠标左键拖动` | 选择 ROI，需先启用 **ROI selection** |
| `Ctrl + Shift + 滚轮` | 调整 UI 字号 |
| `S` | 保存当前视图 |
| `Q` / `Esc` | 退出应用 |

鼠标悬停图像可查看像素值。ROI 面板显示各通道均值、标准差和灰度范围；曝光工具可
标记欠曝与过曝区域。保存的 PNG 和界面配置 `.frame-scope.cfg` 均写入程序启动时的
当前工作目录。

## 在其他工程中发布帧

将 frame-scope 作为子目录引入，并关闭不需要的桌面应用和工具：

```cmake
set(FRAME_SCOPE_BUILD_APP OFF CACHE BOOL "" FORCE)
set(FRAME_SCOPE_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/frame-scope)

target_link_libraries(your_program PRIVATE FrameScope::FrameSender)
```

最小发送示例：

```cpp
#include <opencv2/opencv.hpp>

#include "frame_transport/frame_sender.h"

int main()
{
    frame_scope::FrameSenderOptions options;
    options.endpoint = "tcp://*:5555";
    options.topic = "camera.front";
    options.sourceId = "front-camera";

    frame_scope::FrameSender sender(options);
    cv::VideoCapture camera(0);
    cv::Mat frame;

    while (camera.read(frame))
    {
        sender.publish(frame);
    }
}
```

`FrameSender` 接受 8 位单通道、三通道和四通道图像。默认根据通道数推断 OpenCV
BGR/BGRA 排列，并以 JPEG 编码。可按需调整传输方式：

```cpp
options.encoding = frame_scope::FrameEncoding::Raw;
options.pixelLayout = frame_scope::PixelLayout::Rgb;
options.jpegQuality = 95;
```

发布器默认负责 `bind`。如果端点由其他组件绑定，可改为：

```cpp
options.endpointMode = frame_scope::EndpointMode::Connect;
```

还可以通过 `frame_scope::FrameInfo` 传入帧号、采集时间戳、源帧率、累计丢帧和管线
耗时。详细约束见 `frame_transport/frame_sender.h`。

## 传输协议

标准帧由三个 ZeroMQ message part 组成：

1. UTF-8 topic
2. 序列化的 `frame_scope.FrameMeta`
3. JPEG 或 RAW 图像 payload

协议定义位于 `proto/frame_meta.proto`。H.264 和 H.265 枚举目前仅为预留值，接收端
尚未实现对应的跨帧解码器。

接收端也能解析单段独立图片，或以下形式的连续 `cv::Mat` 数据：

```text
rows cols type\n<pixel bytes>
```

MP4 是视频容器，不能作为单段图片交给 `cv::imdecode`；请通过本地视频拖放功能打开。

## 构建选项

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `FRAME_SCOPE_BUILD_APP` | `ON` | 构建桌面应用、接收端和 UI 库 |
| `FRAME_SCOPE_BUILD_TOOLS` | `ON` | 构建 `SyntheticFramePublisher` |
| `FRAME_SCOPE_BUILD_TESTS` | `OFF` | 构建 GoogleTest 回归测试，需要 GTest |
| `FRAME_SCOPE_BUILD_BENCHMARKS` | `OFF` | 构建多来源接收 benchmark |

只构建发送端：

```bash
cmake -S . -B build-sender \
  -DFRAME_SCOPE_BUILD_APP=OFF \
  -DFRAME_SCOPE_BUILD_TOOLS=OFF
cmake --build build-sender -j
```

## 测试与性能验证

```bash
cmake -S . -B build-test -DCMAKE_BUILD_TYPE=Release \
  -DFRAME_SCOPE_BUILD_TESTS=ON \
  -DFRAME_SCOPE_BUILD_BENCHMARKS=ON
cmake --build build-test -j
ctest --test-dir build-test --output-on-failure
./build-test/MultiSourceReceiverBenchmark
```

回归测试覆盖自动锁定、手动选择、全部摄像头发现，以及“其他来源最后到达时仍返回所选
来源最新帧”的接收行为。benchmark 会对比 payload 复制前过滤与复制后过滤，并在优化
没有显著降低复制字节数时返回失败。

## 升级说明

项目已正式更名为 `frame-scope`，旧名称不再提供兼容入口。集成旧版本时需要同步修改：

| 旧标识 | 当前标识 |
| --- | --- |
| 可执行文件 `FrameVisualizer` | `frame-scope` |
| CMake 选项 `FRAMEVISUALIZER_*` | `FRAME_SCOPE_*` |
| CMake 导入目标 `FrameVisualizer::*` | `FrameScope::*` |
| C++ 命名空间 `frameviz` | `frame_scope` |
| protobuf package `frameviz` | `frame_scope` |
| 配置文件 `.framevis.cfg` | `.frame-scope.cfg` |

不会读取或迁移旧配置文件。

## 项目结构

```text
frame-scope/
├── frame_transport/               # FrameSender、FrameReceiver、来源过滤和 ZMQ 封装
├── ui/                            # SDL2、OpenGL 和 Dear ImGui 界面
├── platform/                      # 平台输入支持
├── proto/frame_meta.proto         # 帧元数据协议
├── tests/                         # 多来源接收回归测试
├── benchmarks/                    # 多来源接收性能验证
├── third_party/imgui/             # 内置 Dear ImGui 源码
├── main.cpp                       # 输入调度、解码、视频播放和主循环
├── synthetic_frame_publisher.cpp  # 合成帧发布器
└── CMakeLists.txt
```

`FrameReceiver` 只负责接收 ZeroMQ 消息；本地视频由 `VideoFilePlayer` 和 OpenCV
`VideoCapture` 读取。两类输入最终写入同一份应用状态，共用显示和分析界面。

## 常见问题

### 窗口一直显示 Waiting for ZMQ image

确认发布端与查看器使用同一端口。发布端通常绑定 `tcp://*:端口`，查看器连接发布主机
的实际 IP；跨主机时还需要检查路由和防火墙。也请确认发送端填写了非空 `topic` 和
`sourceId`。

### Camera 列表中没有目标摄像头

摄像头只有在其 `FrameMeta` 被接收后才会出现在列表中。检查各发布端的 `sourceId`
是否唯一、消息是否持续发布，以及查看器是否连接了正确端点。

### 拖入视频后打开失败

确认文件存在，并且当前 OpenCV 视频后端支持对应容器和编码格式。

### 端到端延迟显示 unavailable

只有发布元数据包含可比较的采集时间戳时才会计算该指标。跨主机使用 Unix Epoch
时间戳时，还需要同步系统时钟。

### 找不到保存的 PNG 或配置文件

文件位于启动程序时的当前工作目录，不一定在可执行文件目录中。

## License

本项目采用 [MIT License](LICENSE)。
