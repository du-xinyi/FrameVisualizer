cmake_minimum_required(VERSION 3.28)

project(
    frame-scope
    VERSION 1.0.0
    DESCRIPTION "OpenCV video and real-time frame analysis tool"
    LANGUAGES CXX
)

option(
    FRAME_SCOPE_BUILD_APP
    "Build the frame-scope desktop application"
    ON
)
option(
    FRAME_SCOPE_BUILD_TOOLS
    "Build frame-scope helper tools"
    ON
)
option(FRAME_SCOPE_BUILD_TESTS "Build frame-scope tests" OFF)
option(FRAME_SCOPE_BUILD_BENCHMARKS "Build frame-scope performance benchmarks" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerOptions)
include(ProjectDependencies)
include(ProtobufGeneration)

# --- FrameProtocol ---
set(FRAME_SCOPE_PROTO_FILE "${PROJECT_SOURCE_DIR}/proto/frame_meta.proto")
frame_scope_generate_protobuf(
    PROTO "${FRAME_SCOPE_PROTO_FILE}"
    OUT_SOURCES FRAME_SCOPE_PROTO_SOURCES
    OUT_HEADERS FRAME_SCOPE_PROTO_HEADERS
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_library(
    FrameProtocol SHARED
    ${FRAME_SCOPE_PROTO_SOURCES}
    ${FRAME_SCOPE_PROTO_HEADERS}
)
add_library(FrameScope::Protocol ALIAS FrameProtocol)

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
        "${FRAME_SCOPE_PROTO_HEADERS}"
        "${CMAKE_BINARY_DIR}/include/protoc/"
    COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different
        "${FRAME_SCOPE_PROTO_FILE}"
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
add_library(FrameScope::FrameSender ALIAS FrameSender)

target_include_directories(FrameSender PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(
    FrameSender
    PUBLIC
        PkgConfig::OPENCV
    PRIVATE
        FrameScope::Protocol
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

# --- frame-scope desktop application ---
if(FRAME_SCOPE_BUILD_APP)
    add_library(
        FrameReceiver SHARED
        frame_transport/frame_receiver.cpp
        frame_transport/frame_receiver.h
        frame_transport/source_filter.cpp
        frame_transport/source_filter.h
    )
    add_library(FrameScope::FrameReceiver ALIAS FrameReceiver)

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
            "${PROJECT_SOURCE_DIR}/frame_transport/source_filter.h"
            "${CMAKE_BINARY_DIR}/include/frame_transport/"
        VERBATIM
    )

    set(
        FRAME_SCOPE_IMGUI_SOURCES
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/imgui_impl_opengl3.cpp
    )

    # ImGui 是内置第三方源码，不继承项目的严格告警策略。
    if(MSVC)
        set_source_files_properties(
            ${FRAME_SCOPE_IMGUI_SOURCES}
            PROPERTIES COMPILE_OPTIONS /w
        )
    else()
        set_source_files_properties(
            ${FRAME_SCOPE_IMGUI_SOURCES}
            PROPERTIES COMPILE_OPTIONS -w
        )
    endif()

    add_library(
        FrameScopeUi SHARED
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
        ${FRAME_SCOPE_IMGUI_SOURCES}
    )
    add_library(FrameScope::Ui ALIAS FrameScopeUi)

    target_include_directories(
        FrameScopeUi
        PUBLIC
            "${PROJECT_SOURCE_DIR}"
        PRIVATE
            "${PROJECT_SOURCE_DIR}/third_party/imgui"
    )
    target_link_libraries(
        FrameScopeUi
        PUBLIC
            PkgConfig::OPENCV
        PRIVATE
            PkgConfig::SDL2
            PkgConfig::OPENGL
    )

    add_custom_command(
        TARGET FrameScopeUi
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

    add_executable(frame-scope main.cpp)
    target_link_libraries(
        frame-scope
        PRIVATE
            FrameScope::Ui
            FrameScope::Protocol
            FrameScope::FrameReceiver
    )

    if(FRAME_SCOPE_BUILD_TESTS)
        enable_testing()
        find_package(GTest REQUIRED)
        include(GoogleTest)

        add_executable(
            FrameReceiverTests
            tests/frame_receiver_test.cpp
        )
        target_link_libraries(
            FrameReceiverTests
            PRIVATE
                FrameScope::FrameReceiver
                FrameScope::Protocol
                FrameZmqSupport
                GTest::gtest_main
        )
        gtest_discover_tests(FrameReceiverTests)
    endif()

    if(FRAME_SCOPE_BUILD_BENCHMARKS)
        add_executable(
            MultiSourceReceiverBenchmark
            benchmarks/multi_source_receiver_benchmark.cpp
        )
        target_link_libraries(
            MultiSourceReceiverBenchmark
            PRIVATE
                FrameScope::FrameReceiver
                FrameScope::Protocol
                FrameZmqSupport
        )
    endif()
endif()

# --- SyntheticFramePublisher ---
if(FRAME_SCOPE_BUILD_TOOLS)
    add_executable(SyntheticFramePublisher synthetic_frame_publisher.cpp)
    target_link_libraries(
        SyntheticFramePublisher
        PRIVATE
            FrameScope::FrameSender
    )
endif()
