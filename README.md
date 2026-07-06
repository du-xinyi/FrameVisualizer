# FrameVisualizer

FrameVisualizer 是一个面向 OpenCV 图像和视频的 Linux 桌面查看器，支持两类输入：

- 将本地视频文件直接拖入窗口播放
- 通过 ZeroMQ 接收实时 `cv::Mat` 图像流

应用使用 SDL2、OpenGL 和 Dear ImGui 绘制界面，提供通道查看、像素检查、ROI 分析、
直方图、曝光警告和异步图像保存。项目还包含可嵌入其他 CMake 工程的
`FrameSender` 库及合成帧发布工具。

## 功能

### 本地视频

- 将 MP4 等 OpenCV `VideoCapture` 支持的文件拖入播放窗口
- 按视频帧率播放，支持 `0.25x`、`0.5x`、`1x`、`1.5x`、`2x` 和 `4x`
- 使用底部时间轴按帧定位
- 暂停状态下仍可拖动时间轴或使用方向键查看目标帧
- 方向键支持逐帧、长按加速和 `Shift` 快速移动
- 右上角 `X` 关闭视频并返回当前 ZMQ 输入

### 实时图像流

- 接收 `topic + FrameMeta + payload` 三段 ZMQ 消息
- 解码 JPEG 和 RAW 负载
- 兼容单段压缩图像以及 `rows cols type\n` 开头的原始 `cv::Mat` 消息
- 后台解码并覆盖积压任务，以最新帧优先保证低延迟
- 显示源帧率、接收帧率、渲染帧率、跳帧和链路延迟
- 暂停后浏览最近 60 帧，同时受 256 MiB 内存预算限制
- 在运行期间切换 ZMQ 订阅端点

### 查看与分析

- RGB、灰度、HSV 及单通道视图
- 线性、对数和累计直方图
- 像素悬停采样
- ROI 均值、标准差和灰度范围分析
- 欠曝与过曝标记
- 异步保存原始帧、当前视图或直方图为 PNG

## 环境要求

- Linux
- 支持 C++20 的 GCC 或 Clang
- CMake 3.28+
- pkg-config
- OpenCV 4，包含所需视频解码后端
- libzmq
- protobuf 库与 `protoc`
- SDL2 和 OpenGL，桌面应用构建时需要

Ubuntu/Debian 可安装：

```bash
sudo apt update
sudo apt install build-essential cmake pkg-config \
  libopencv-dev libzmq3-dev \
  libprotobuf-dev protobuf-compiler \
  libsdl2-dev libgl1-mesa-dev
```

Dear ImGui 源码已经包含在 `third_party/imgui`，无需单独安装。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

默认目标：

| 目标 | 用途 |
| --- | --- |
| `FrameVisualizer` | 本地视频和实时图像流查看器 |
| `SyntheticFramePublisher` | 生成 1280x720、30 FPS 的合成 JPEG 流 |
| `FrameSender` | 发布 `cv::Mat` 的动态库 |
| `FrameReceiver` | 接收最新 ZMQ multipart 消息的动态库 |
| `FrameProtocol` | protobuf 协议动态库 |

只构建发送端库：

```bash
cmake -S . -B build \
  -DFRAMEVISUALIZER_BUILD_APP=OFF \
  -DFRAMEVISUALIZER_BUILD_TOOLS=OFF
cmake --build build -j
```

## 查看本地视频

启动查看器：

```bash
./build/FrameVisualizer
```

将视频文件从文件管理器拖入播放窗口。成功打开后，窗口底部显示时间轴、当前时间、
总时长和播放倍速，右上角显示关闭按钮。

可用控制：

| 操作 | 功能 |
| --- | --- |
| 拖动底部时间轴 | 按帧定位 |
| `Space` | 暂停或继续播放 |
| `Left` / `Right` | 后退或前进 1 帧 |
| 长按方向键 0.5～1.5 秒 | 每次键盘重复移动 5 帧 |
| 长按方向键超过 1.5 秒 | 每次键盘重复移动 10 帧 |
| `Shift + Left` / `Shift + Right` | 后退或前进 5 帧 |
| 倍速菜单 | 选择 `0.25x` 至 `4x` 播放速度 |
| 右上角 `X` | 关闭视频并返回 ZMQ 输入 |

实际可打开的视频容器和编码格式由当前 OpenCV `VideoCapture` 后端决定。无法打开时，
界面状态会显示失败原因。

## 查看实时图像流

先启动合成帧发布器：

```bash
./build/SyntheticFramePublisher
```

发布器默认绑定 `tcp://*:5555`，主题为 `sim.camera.0`。在另一个终端启动查看器：

```bash
./build/FrameVisualizer
```

查看器默认连接 `tcp://127.0.0.1:5555`。也可以通过第一个参数指定端点：

```bash
./build/FrameVisualizer tcp://192.168.1.20:5555
./build/SyntheticFramePublisher tcp://*:6000
```

端点可在 ZMQ 模式的 **Connection** 区域修改。本地视频播放期间该区域会隐藏；使用
右上角 `X` 返回 ZMQ 模式后即可调整端点。

> ZMQ PUB/SUB 不会补发订阅握手完成前的消息，启动阶段丢失少量帧属于正常现象。

## 通用操作

| 操作 | 功能 |
| --- | --- |
| `1` / `2` / `3` | RGB / 灰度 / HSV 视图 |
| `4` / `5` / `6` | 红/绿/蓝通道，或 H/S/V 通道 |
| `H` | 开关线性直方图 |
| `Space` | 暂停或继续采用新帧 |
| `F` | 开关自适应窗口 |
| `+` / `-` | 放大或缩小 |
| `Ctrl + 滚轮` | 以鼠标位置为中心缩放 |
| 鼠标左键拖动 | 平移手动缩放后的图像 |
| `Shift + 鼠标左键拖动` | 选择 ROI，需先启用 ROI selection |
| `Ctrl + Shift + 滚轮` | 调整 UI 字号 |
| `S` | 保存当前视图 |
| `Q` / `Esc` | 退出应用 |

保存文件写入程序当前工作目录，文件名包含类型、时间和序号。窗口尺寸、控制面板比例
和 UI 字号保存在当前工作目录的 `.framevis.cfg`。

## 在其他工程中发送帧

将本项目作为子目录引入并链接 `FrameSender`：

```cmake
set(FRAMEVISUALIZER_BUILD_APP OFF CACHE BOOL "" FORCE)
set(FRAMEVISUALIZER_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/FrameVisualizer)

target_link_libraries(your_program PRIVATE FrameSender)
```

最小发送示例：

```cpp
#include <opencv2/opencv.hpp>

#include "frame_transport/frame_sender.h"

int main()
{
    frameviz::FrameSenderOptions options;
    options.endpoint = "tcp://*:5555";
    options.topic = "camera.front";
    options.sourceId = "front-camera";

    frameviz::FrameSender sender(options);
    cv::VideoCapture camera(0);
    cv::Mat frame;

    while (camera.read(frame))
    {
        sender.publish(frame);
    }
}
```

`FrameSender` 接受 8 位单通道、三通道和四通道图像。默认按照 OpenCV 的 BGR/BGRA
排列解释输入并编码为 JPEG。需要未压缩传输时可使用 RAW：

```cpp
options.encoding = frameviz::FrameEncoding::Raw;
```

发布器默认负责 `bind`。如果端点由其他组件绑定：

```cpp
options.endpointMode = frameviz::EndpointMode::Connect;
```

`frameviz::FrameInfo` 可随帧传入帧号、采集时间戳、源帧率、累计丢帧和管线耗时。
公开接口及约束见 `frame_transport/frame_sender.h`。

## 传输协议

标准消息包含三个 ZMQ message part：

1. UTF-8 topic
2. 序列化的 `frameviz.FrameMeta`
3. JPEG 或 RAW 图像负载

协议定义位于 `proto/frame_meta.proto`。H.264 和 H.265 枚举目前仅为预留值，接收端
尚未实现对应的跨帧解码器。

兼容的单段消息可以是 `cv::imdecode` 支持的独立图片，或者以下格式的连续 Mat 数据：

```text
rows cols type\n<pixel bytes>
```

MP4 是视频容器，不能作为单段图像消息交给 `cv::imdecode`。本地 MP4 由
`cv::VideoCapture` 独立处理。

## 代码结构

```text
FrameVisualizer/
├── frame_transport/               # FrameSender、FrameReceiver 和 ZMQ RAII 封装
├── ui/                            # SDL2、OpenGL 和 Dear ImGui 界面
├── platform/                      # 终端键盘输入
├── proto/frame_meta.proto         # 帧传输元数据协议
├── third_party/imgui/             # 内置 Dear ImGui 源码
├── main.cpp                       # 输入调度、解码、视频播放和应用主循环
├── synthetic_frame_publisher.cpp  # 合成帧发布工具
└── CMakeLists.txt
```

输入职责保持分离：`FrameReceiver` 只接收 ZMQ 消息，本地视频由 `VideoFilePlayer` 和
OpenCV `VideoCapture` 读取。两类输入最终都写入相同的 `AppState`，复用查看与分析 UI。

## 常见问题

### 拖入视频后显示打开失败

确认文件存在且当前 OpenCV 视频后端支持该容器和编码。可先使用其他 OpenCV 程序或
系统工具验证视频是否能够解码。

### 窗口一直显示 Waiting for ZMQ image

确认发布端和查看器端口一致。发布端通常使用 `tcp://*:端口` 绑定，查看器使用发布
主机的实际 IP 连接；跨主机连接还需检查防火墙。

### CMake 找不到依赖

确认已安装对应开发包和 `pkg-config`。protobuf 同时需要库和 `protoc` 编译器。

### 端到端延迟显示 unavailable

只有发布元数据包含可比较的采集时间戳时才会计算该指标。跨主机使用 Unix Epoch
时间戳时还需要同步系统时钟。

### 找不到保存的 PNG 或配置文件

PNG 和 `.framevis.cfg` 都写入启动程序时的当前工作目录，不一定是可执行文件目录。
