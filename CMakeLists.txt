cmake_minimum_required(VERSION 3.20)
project(gow4_autoparry CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # 静态 CRT，注入进游戏不依赖运行库

# 注入器
add_executable(gow4_inject src/inject.cpp)
target_compile_definitions(gow4_inject PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(gow4_inject PRIVATE /utf-8 /W4)

# QTE 探针：hook 提示输入判定，只记录
add_library(gow4_probe SHARED src/probe/qte_probe.cpp)
target_compile_definitions(gow4_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(gow4_probe PRIVATE /utf-8 /W4)
add_dependencies(gow4_inject gow4_probe gow4_esp gow4_blockprobe)

# 格挡探针：hook 命中结算里的格挡判定 0x79f0c0，只记录
add_library(gow4_blockprobe SHARED src/probe/block_probe.cpp)
target_compile_definitions(gow4_blockprobe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(gow4_blockprobe PRIVATE /utf-8 /W4)

# 自动弹反实验：命中结算时把奎托斯的防御组临时换成弹反体
add_library(gow4_autoparry SHARED src/autoparry/autoparry.cpp)
target_compile_definitions(gow4_autoparry PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(gow4_autoparry PRIVATE /utf-8 /W4)
add_dependencies(gow4_inject gow4_autoparry)

# 透视：hook DXGI Present，用 ImGui 把附近可采集物画到游戏画面上
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMGUI_SRC ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
set_source_files_properties(${IMGUI_SRC} PROPERTIES COMPILE_OPTIONS "/W0")
set_source_files_properties(src/esp/esp.cpp PROPERTIES COMPILE_OPTIONS "/W4")
add_library(gow4_esp SHARED src/esp/esp.cpp ${IMGUI_SRC})
target_include_directories(gow4_esp PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(gow4_esp PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(gow4_esp PRIVATE /utf-8)
target_link_libraries(gow4_esp PRIVATE d3d11 dxgi d3dcompiler)
add_custom_command(TARGET gow4_esp POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/src/esp/gow4_esp.ini $<TARGET_FILE_DIR:gow4_esp>/gow4_esp.ini)

# 透视 DLL 的测试宿主（不碰游戏）
add_executable(esp_host tests/esp_host.cpp)
target_compile_definitions(esp_host PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(esp_host PRIVATE /utf-8)
