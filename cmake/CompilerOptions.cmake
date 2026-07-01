# === 编译选项 ===
set(CMAKE_CXX_STANDARD 20) # C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON) # 强制要求编译器必须支持指定标准
set(CMAKE_CXX_EXTENSIONS OFF) # 禁用 GNU 扩展
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # 导出 compile_commands.json
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # 生成位置无关代码

# === 编译警告 ===
add_compile_options(
    # 常用警告
    -Wall
    -Wextra
    -Wpedantic
    # 类型转换警告
    -Wconversion
    -Wsign-conversion
    # 未初始化变量
    -Wuninitialized
    # 虚函数析构
    -Wnon-virtual-dtor
    # 重写虚函数检查
    -Woverloaded-virtual
    # 禁止未知 pragma
    -Wunknown-pragmas
    # 格式化字符串检查
    -Wformat=2)

# Debug 模式（关闭优化 + 完整调试信息）
add_compile_options(
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-g3>
)

# Release 模式（性能优先）
add_compile_options(
    $<$<CONFIG:Release>:-O3>
)

# RelWithDebInfo 模式（平衡优化 + 调试信息）
add_compile_options(
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-g>
)

# MinSizeRel 模式（体积优先）
add_compile_options(
    $<$<CONFIG:MinSizeRel>:-Os>
)
