include_guard(GLOBAL)

# 为单个 .proto 文件生成 C++ 源码。
#
# framevisualizer_generate_protobuf(
#     PROTO <file>
#     OUT_SOURCES <variable>
#     OUT_HEADERS <variable>
#     [OUTPUT_DIRECTORY <directory>]
# )
function(framevisualizer_generate_protobuf)
    cmake_parse_arguments(
        PARSE_ARGV 0
        ARG
        ""
        "PROTO;OUT_SOURCES;OUT_HEADERS;OUTPUT_DIRECTORY"
        ""
    )

    if(ARG_UNPARSED_ARGUMENTS)
        message(
            FATAL_ERROR
            "framevisualizer_generate_protobuf received unknown arguments: "
            "${ARG_UNPARSED_ARGUMENTS}"
        )
    endif()

    foreach(_required_argument IN ITEMS PROTO OUT_SOURCES OUT_HEADERS)
        if(NOT ARG_${_required_argument})
            message(
                FATAL_ERROR
                "framevisualizer_generate_protobuf requires ${_required_argument}"
            )
        endif()
    endforeach()

    get_filename_component(_proto_absolute "${ARG_PROTO}" ABSOLUTE)
    if(NOT EXISTS "${_proto_absolute}")
        message(FATAL_ERROR "Protobuf schema not found: ${_proto_absolute}")
    endif()

    get_filename_component(_proto_directory "${_proto_absolute}" DIRECTORY)
    get_filename_component(_proto_name "${_proto_absolute}" NAME_WE)

    if(ARG_OUTPUT_DIRECTORY)
        get_filename_component(
            _output_directory
            "${ARG_OUTPUT_DIRECTORY}"
            ABSOLUTE
        )
    else()
        set(_output_directory "${CMAKE_CURRENT_BINARY_DIR}")
    endif()

    set(_generated_source "${_output_directory}/${_proto_name}.pb.cc")
    set(_generated_header "${_output_directory}/${_proto_name}.pb.h")

    if(TARGET protobuf::protoc)
        set(_protoc_command "$<TARGET_FILE:protobuf::protoc>")
        set(_protoc_dependency protobuf::protoc)
    elseif(Protobuf_PROTOC_EXECUTABLE)
        set(_protoc_command "${Protobuf_PROTOC_EXECUTABLE}")
        set(_protoc_dependency "${Protobuf_PROTOC_EXECUTABLE}")
    else()
        message(FATAL_ERROR "Protobuf compiler 'protoc' was not found")
    endif()

    add_custom_command(
        OUTPUT
            "${_generated_source}"
            "${_generated_header}"
        COMMAND
            "${CMAKE_COMMAND}" -E make_directory "${_output_directory}"
        COMMAND
            "${_protoc_command}"
            "--cpp_out=${_output_directory}"
            "--proto_path=${_proto_directory}"
            "${_proto_absolute}"
        DEPENDS
            "${_proto_absolute}"
            ${_protoc_dependency}
        COMMENT "Generating C++ protocol sources from ${_proto_name}.proto"
        VERBATIM
    )

    set_source_files_properties(
        "${_generated_source}"
        "${_generated_header}"
        PROPERTIES GENERATED TRUE
    )

    set(${ARG_OUT_SOURCES} "${_generated_source}" PARENT_SCOPE)
    set(${ARG_OUT_HEADERS} "${_generated_header}" PARENT_SCOPE)
endfunction()
