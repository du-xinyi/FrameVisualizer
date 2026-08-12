cmake_minimum_required(VERSION 3.28)

project(
    FrameVisualizer
    VERSION 1.0.0
    DESCRIPTION "Real-time OpenCV frame stream visualizer"
    LANGUAGES CXX
)

option(FRAMEVISUALIZER_BUILD_APP "Build the FrameVisualizer desktop application" ON)
option(FRAMEVISUALIZER_BUILD_TOOLS "Build FrameVisualizer helper tools" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerOptions)
include(ProjectDependencies)
include(ProtobufGeneration)

# --- FrameProtocol ---
set(FRAMEVISUALIZER_PROTO_FILE "${PROJECT_SOURCE_DIR}/proto/frame_meta.proto")
framevisualizer_generate_protobuf(
    PROTO "${FRAMEVISUALIZER_PROTO_FILE}"
    OUT_SOURCES FRAMEVISUALIZER_PROTO_SOURCES
    OUT_HEADERS FRAMEVISUALIZER_PROTO_HEADERS
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_library(
    FrameProtocol SHARED
    ${FRAMEVISUALIZER_PROTO_SOURCES}
    ${FRAMEVISUALIZER_PROTO_HEADERS}
)
add_library(FrameVisualizer::Protocol ALIAS FrameProtocol)

target_compile_features(FrameProtocol PUBLIC cxx_std_20)
target_include_directories(FrameProtocol PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(FrameProtocol PUBLIC protobuf::libprotobuf PkgConfig::PROTOBUF_LIBS)

add_custom_command(
    TARGET FrameProtocol
    POST_BUILD
    COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_BINARY_DIR}/include/protoc"
    COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different
        "${FRAMEVISUALIZER_PROTO_HEADERS}"
        "${CMAKE_BINARY_DIR}/include/protoc/"
    COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different
        "${FRAMEVISUALIZER_PROTO_FILE}"
        "${CMAKE_BINARY_DIR}/include/protoc/"
    VERBATIM
)

# --- Internal libzmq support ---
add_library(
    FrameZmqSupport STATIC
    frame_transport/zmq_raii.cpp
    frame_transport/zmq_raii.h
)
target_include_directories(FrameZmqSupport PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(FrameZmqSupport PUBLIC PkgConfig::LIBZMQ)
set_target_properties(
    FrameZmqSupport
    PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
)

# --- FrameSender ---
add_library(
    FrameSender SHARED
    frame_transport/frame_sender.cpp
    frame_transport/frame_sender.h
)
add_library(FrameVisualizer::FrameSender ALIAS FrameSender)

target_include_directories(FrameSender PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(
    FrameSender
    PUBLIC
        PkgConfig::OPENCV
    PRIVATE
        FrameVisualizer::Protocol
        FrameZmqSupport
)

add_custom_command(
    TARGET FrameSender
    POST_BUILD
    COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_BINARY_DIR}/include/frame_transport"
    COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/frame_transport/frame_sender.h"
        "${CMAKE_BINARY_DIR}/include/frame_transport/"
    VERBATIM
)

# --- FrameVisualizer desktop application ---
if(FRAMEVISUALIZER_BUILD_APP)
    add_library(
        FrameReceiver SHARED
        frame_transport/frame_receiver.cpp
        frame_transport/frame_receiver.h
    )
    add_library(FrameVisualizer::FrameReceiver ALIAS FrameReceiver)

    target_include_directories(FrameReceiver PUBLIC "${PROJECT_SOURCE_DIR}")
    target_link_libraries(FrameReceiver PRIVATE FrameZmqSupport)

    add_custom_command(
        TARGET FrameReceiver
        POST_BUILD
        COMMAND
            "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_BINARY_DIR}/include/frame_transport"
        COMMAND
            "${CMAKE_COMMAND}" -E copy_if_different
            "${PROJECT_SOURCE_DIR}/frame_transport/frame_receiver.h"
            "${CMAKE_BINARY_DIR}/include/frame_transport/"
        VERBATIM
    )

    set(
        FRAMEVISUALIZER_IMGUI_SOURCES
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/imgui_impl_opengl3.cpp
    )

    # ImGui 是内置第三方源码，不继承项目的严格告警策略。
    if(MSVC)
        set_source_files_properties(
            ${FRAMEVISUALIZER_IMGUI_SOURCES}
            PROPERTIES COMPILE_OPTIONS /w
        )
    else()
        set_source_files_properties(
            ${FRAMEVISUALIZER_IMGUI_SOURCES}
            PROPERTIES COMPILE_OPTIONS -w
        )
    endif()

    add_library(
        FrameVisualizerUi SHARED
        ui/async_image_saver.cpp
        ui/async_image_saver.h
        ui/ui.cpp
        ui/ui.h
        ui/ui_config.cpp
        ui/ui_config.h
        ui/ui_controls.cpp
        ui/ui_context_impl.h
        ui/ui_image_processing.cpp
        ui/ui_image_processing.h
        ui/ui_input.cpp
        ui/ui_input.h
        ui/ui_layout.h
        ui/ui_viewport.cpp
        platform/keyboard_listener.cpp
        platform/keyboard_listener.h
        ${FRAMEVISUALIZER_IMGUI_SOURCES}
    )
    add_library(FrameVisualizer::Ui ALIAS FrameVisualizerUi)

    target_include_directories(
        FrameVisualizerUi
        PUBLIC
            "${PROJECT_SOURCE_DIR}"
        PRIVATE
            "${PROJECT_SOURCE_DIR}/third_party/imgui"
    )
    target_link_libraries(
        FrameVisualizerUi
        PUBLIC
            PkgConfig::OPENCV
        PRIVATE
            PkgConfig::SDL2
            PkgConfig::OPENGL
    )

    add_custom_command(
        TARGET FrameVisualizerUi
        POST_BUILD
        COMMAND
            "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_BINARY_DIR}/include/ui"
        COMMAND
            "${CMAKE_COMMAND}" -E copy_if_different
            "${PROJECT_SOURCE_DIR}/ui/ui.h"
            "${CMAKE_BINARY_DIR}/include/ui/"
        VERBATIM
    )

    add_executable(FrameVisualizer main.cpp)
    target_link_libraries(
        FrameVisualizer
        PRIVATE
            FrameVisualizer::Ui
            FrameVisualizer::Protocol
            FrameVisualizer::FrameReceiver
    )
endif()

# --- SyntheticFramePublisher ---
if(FRAMEVISUALIZER_BUILD_TOOLS)
    add_executable(SyntheticFramePublisher synthetic_frame_publisher.cpp)
    target_link_libraries(
        SyntheticFramePublisher
        PRIVATE
            FrameVisualizer::FrameSender
    )
endif()
