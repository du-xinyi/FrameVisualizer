include_guard(GLOBAL)

# === 输出目录 ===
# set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin) # 可执行文件
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib) # 动态库
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib) # 静态库

# 当前项目使用系统依赖。这里只负责发现依赖并创建导入目标，具体链接关系仍由
# 根 CMakeLists.txt 中的项目目标声明。
find_package(PkgConfig REQUIRED)
find_package(Protobuf REQUIRED)

pkg_check_modules(OPENCV REQUIRED IMPORTED_TARGET opencv4)
pkg_check_modules(LIBZMQ REQUIRED IMPORTED_TARGET libzmq)
pkg_check_modules(PROTOBUF_LIBS REQUIRED IMPORTED_TARGET protobuf)

# FindProtobuf 的模块模式和配置模式均应提供这些标准目标。提前检查可以给出比
# target_link_libraries 阶段更明确的错误。
if(NOT TARGET protobuf::libprotobuf)
    message(FATAL_ERROR "Protobuf package does not provide protobuf::libprotobuf")
endif()

if(NOT TARGET protobuf::protoc AND NOT Protobuf_PROTOC_EXECUTABLE)
    message(FATAL_ERROR "Protobuf compiler 'protoc' was not found")
endif()

if(FRAME_SCOPE_BUILD_APP)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(OPENGL REQUIRED IMPORTED_TARGET gl)
endif()

message(
    STATUS
    "frame-scope dependencies: OpenCV ${OPENCV_VERSION}, "
    "ZeroMQ ${LIBZMQ_VERSION}, Protobuf ${Protobuf_VERSION}"
)
