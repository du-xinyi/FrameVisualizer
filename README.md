# FrameVisualizer

FrameVisualizer 是一个面向 OpenCV 图像流的 Linux 桌面调试工具。它通过 ZeroMQ
接收图像帧，解析 protobuf 元数据，并提供低延迟预览、历史帧浏览、像素与 ROI
分析、直方图和图像保存功能。

项目同时提供可嵌入其他 CMake 工程的 `FrameSender` 库，以及一个无需相机即可验证
完整链路的合成帧发布器。

## 功能

- 接收 `topic + FrameMeta + payload` 三段 ZMQ 消息
- 支持 JPEG 和 RAW 负载，以及兼容的单段压缩图像/原始 `cv::Mat` 消息
- 后台解码并主动丢弃积压帧，优先保持实时预览的低延迟
- 显示源帧率、接收帧率、渲染帧率、跳帧数和各阶段延迟
- RGB、灰度、HSV 及单通道视图
- 线性、对数和累计直方图
- 像素悬停采样、ROI 均值/标准差/灰度范围分析和曝光警告
- 暂停后浏览最近 60 帧（同时受 256 MiB 内存预算限制）
- 异步保存原始帧、当前视图或直方图为 PNG
- 运行期间切换订阅端点

## 依赖

- Linux，支持 C++20 的 GCC 或 Clang
- CMake 3.28+
- pkg-config
- OpenCV 4
- libzmq
- protobuf（库与编译器）
- SDL2 和 OpenGL（仅桌面应用需要）

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

默认生成以下目标：

| 目标 | 用途 |
| --- | --- |
| `FrameVisualizer` | 图像流查看器 |
| `SyntheticFramePublisher` | 1280×720、30 FPS 的合成 JPEG 流发布器 |
| `FrameSender` | 向查看器发布 `cv::Mat` 的动态库 |
| `FrameReceiver` | 接收最新 ZMQ multipart 消息的动态库（随桌面应用构建） |
| `FrameProtocol` | protobuf 协议动态库 |

可通过 CMake 选项裁剪构建：

```bash
# 只构建发送端库，不构建 UI 和示例工具
cmake -S . -B build \
  -DFRAMEVISUALIZER_BUILD_APP=OFF \
  -DFRAMEVISUALIZER_BUILD_TOOLS=OFF
cmake --build build -j
```

## 快速开始

先启动合成帧发布器：

```bash
./build/SyntheticFramePublisher
```

它默认绑定 `tcp://*:5555`，主题为 `sim.camera.0`。在另一个终端启动查看器：

```bash
./build/FrameVisualizer
```

查看器默认连接 `tcp://127.0.0.1:5555`。也可以将其他端点作为第一个参数传入：

```bash
./build/FrameVisualizer tcp://192.168.1.20:5555
./build/SyntheticFramePublisher tcp://*:6000
```

端点也可在界面的 **Connection** 区域修改。查看器订阅所有主题，界面会显示当前帧
携带的 topic 和 source ID。

> ZMQ PUB/SUB 不会补发订阅握手完成前的消息，启动阶段丢失少量帧属于正常现象。

## 操作

| 操作 | 功能 |
| --- | --- |
| `1` / `2` / `3` | RGB / 灰度 / HSV 视图 |
| `4` / `5` / `6` | 红/绿/蓝通道，或 H/S/V 通道 |
| `H` | 开关线性直方图 |
| `Space` | 暂停或继续接收；暂停后可在控制面板浏览历史帧 |
| `F` | 开关自适应窗口 |
| `+` / `-` | 放大或缩小 |
| `Ctrl + 滚轮` | 以鼠标位置为中心缩放 |
| 鼠标左键拖动 | 平移手动缩放后的图像 |
| `Shift + 鼠标左键拖动` | 选择 ROI（需先启用 ROI selection） |
| `Ctrl + Shift + 滚轮` | 调整 UI 字号 |
| `S` | 保存当前视图 |
| `Q` / `Esc` | 退出 |

保存文件写入程序的当前工作目录，文件名包含类型、时间和序号，例如
`view_frame_20260629_120000_000_0.png`。窗口尺寸、控制面板比例和 UI 字号保存在
当前工作目录的 `.framevis.cfg`。

## 在其他工程中发送帧

将本项目作为子目录引入并链接实际存在的 `FrameSender` 目标：

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

`FrameSender` 接受 8 位的单通道、三通道和四通道图像，默认按 OpenCV 的 BGR/BGRA
排列解释并编码为 JPEG。需要无损传输时可改用 RAW：

```cpp
options.encoding = frameviz::FrameEncoding::Raw;
```

发布器默认 `bind`。如果另一端负责绑定，可设置：

```cpp
options.endpointMode = frameviz::EndpointMode::Connect;
```

动态帧号、采集时间戳、源帧率和链路耗时可通过 `frameviz::FrameInfo` 随每帧传入。
完整接口说明见 [docs/frame_sender.md](docs/frame_sender.md)。

## 传输协议

标准消息由三个 ZMQ message part 组成：

1. UTF-8 topic
2. 序列化的 `frameviz.FrameMeta`
3. JPEG 或 RAW 图像负载

协议定义位于 [proto/frame_meta.proto](proto/frame_meta.proto)。当前接收器实现 JPEG 和
RAW 解码；协议中的 H.264/H.265 枚举为预留值，尚未实现。

为兼容旧发送端，接收器也接受单段消息：可由 `cv::imdecode` 解码的图像字节，或
`rows cols type\n` 后紧跟连续像素数据的原始 `cv::Mat` 格式。

## 目录结构

```text
FrameVisualizer/
├── frame_transport/               # cv::Mat 发送库、消息接收器及内部 ZMQ 封装
├── ui/                            # SDL2、OpenGL 和 ImGui 界面
├── platform/                      # 平台输入实现
├── proto/frame_meta.proto         # 帧元数据协议
├── docs/frame_sender.md           # 发送端接口说明
├── third_party/imgui/             # 内置 Dear ImGui 源码
├── main.cpp                       # 查看器入口与接收/解码流程
├── synthetic_frame_publisher.cpp  # 合成帧发布器
└── CMakeLists.txt
```

## 常见问题

- **窗口一直显示 Waiting for ZMQ image**：确认发布端和查看器端口一致，并检查防火墙；
  发布端使用 `tcp://*:端口` 绑定，查看器使用发布主机的实际 IP 连接。
- **CMake 找不到 libzmq**：确认已安装 `libzmq3-dev` 和 `pkg-config`。
- **端到端延迟显示 unavailable**：只有采集时间戳与接收端可比较时才会计算该指标；
  跨主机使用 Unix Epoch 时间戳时还需要保证系统时钟同步。
- **保存位置不明确**：PNG 与 `.framevis.cfg` 都使用启动程序时的当前工作目录，
  不一定是可执行文件所在目录。
