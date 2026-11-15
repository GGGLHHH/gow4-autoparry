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
target_include_directories(gow4_autoparry PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(gow4_autoparry PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(gow4_autoparry PRIVATE /utf-8 /W4)
add_dependencies(gow4_inject gow4_autoparry)
# ini 只在输出目录里还没有时才放一份默认的：那里的 ini 是用户 / 修改器改过的，重编不能盖掉
add_custom_command(TARGET gow4_autoparry POST_BUILD COMMAND ${CMAKE_COMMAND}
    -DSRC=${CMAKE_SOURCE_DIR}/src/autoparry/gow4_autoparry.ini -DDST=$<TARGET_FILE_DIR:gow4_autoparry>/gow4_autoparry.ini
    -P ${CMAKE_SOURCE_DIR}/cmake/copy_if_missing.cmake)

# 透视：hook DXGI Present，用 ImGui 把附近可采集物画到游戏画面上
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMGUI_SRC ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
set_source_files_properties(${IMGUI_SRC} PROPERTIES COMPILE_OPTIONS "/W0")
set_source_files_properties(src/esp/esp.cpp PROPERTIES COMPILE_OPTIONS "/W4")
add_library(gow4_esp SHARED src/esp/esp.cpp ${IMGUI_SRC})
target_include_directories(gow4_esp PRIVATE ${CMAKE_SOURCE_DIR}/src ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(gow4_esp PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(gow4_esp PRIVATE /utf-8)
target_link_libraries(gow4_esp PRIVATE d3d11 dxgi d3dcompiler)
add_custom_command(TARGET gow4_esp POST_BUILD COMMAND ${CMAKE_COMMAND}
    -DSRC=${CMAKE_SOURCE_DIR}/src/esp/gow4_esp.ini -DDST=$<TARGET_FILE_DIR:gow4_esp>/gow4_esp.ini
    -P ${CMAKE_SOURCE_DIR}/cmake/copy_if_missing.cmake)

# 修改器本体（界面）：外部程序，透明覆盖层上画菜单；找游戏、注入 / 卸载、把设置写进两个 DLL 的 ini。见 docs/plan.md
set(TRAINER_SOURCES
    src/trainer/main.cpp
    src/trainer/overlay.cpp
    src/trainer/widgets.cpp
    src/trainer/settings.cpp
    src/trainer/game.cpp
    src/trainer/injector.cpp
    src/trainer/dll_files.cpp
    src/trainer/menu.cpp
    src/trainer/pages.cpp)
# WIN32 = 窗口程序，不弹控制台。不带 imgui_demo
add_executable(gow4_trainer WIN32 ${TRAINER_SOURCES}
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(gow4_trainer PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/trainer ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(gow4_trainer PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(gow4_trainer PRIVATE /utf-8 /MP)
set_source_files_properties(${TRAINER_SOURCES} PROPERTIES COMPILE_OPTIONS "/W4")
target_link_libraries(gow4_trainer PRIVATE d3d11 dxgi dwmapi windowscodecs ole32)

# 便携版（build.ps1 -Dist 会打开）：两个 DLL 编进 gow4_trainer.exe，发出去只要一个文件；
# 启动时由 dll_files.cpp 释放到 %LOCALAPPDATA%\gow4_trainer\，ini 也放那里。DLL 重编后这里跟着重新生成。
# 开发版不内嵌：修改器用 exe 旁边单独编出来的 DLL，编修改器不会连带重编 DLL
option(GOW4_PORTABLE "Embed the two DLLs into gow4_trainer.exe" OFF)
if(GOW4_PORTABLE)
    target_compile_definitions(gow4_trainer PRIVATE GOW4_PORTABLE)
    set(PARRY_DLL_BLOB ${CMAKE_BINARY_DIR}/generated/parry_dll_blob.cpp)
    set(ESP_DLL_BLOB ${CMAKE_BINARY_DIR}/generated/esp_dll_blob.cpp)
    add_custom_command(
        OUTPUT ${PARRY_DLL_BLOB}
        COMMAND ${CMAKE_COMMAND} -DIN=$<TARGET_FILE:gow4_autoparry> -DOUT=${PARRY_DLL_BLOB} -DNAME=kParryDllBlob
                -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
        DEPENDS gow4_autoparry $<TARGET_FILE:gow4_autoparry> ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
        COMMENT "Embedding gow4_autoparry.dll"
        VERBATIM)
    add_custom_command(
        OUTPUT ${ESP_DLL_BLOB}
        COMMAND ${CMAKE_COMMAND} -DIN=$<TARGET_FILE:gow4_esp> -DOUT=${ESP_DLL_BLOB} -DNAME=kEspDllBlob
                -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
        DEPENDS gow4_esp $<TARGET_FILE:gow4_esp> ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
        COMMENT "Embedding gow4_esp.dll"
        VERBATIM)
    target_sources(gow4_trainer PRIVATE ${PARRY_DLL_BLOB} ${ESP_DLL_BLOB})
endif()

# 修改器的测试（控制台程序）：配置读写。全在临时目录里做，不开窗口、不碰游戏
add_executable(trainer_tests tests/trainer_tests.cpp src/trainer/settings.cpp)
target_include_directories(trainer_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/trainer)
target_compile_definitions(trainer_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(trainer_tests PRIVATE /utf-8 /W4)

# 透视 DLL 的测试宿主（不碰游戏）
add_executable(esp_host tests/esp_host.cpp)
target_compile_definitions(esp_host PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(esp_host PRIVATE /utf-8)
