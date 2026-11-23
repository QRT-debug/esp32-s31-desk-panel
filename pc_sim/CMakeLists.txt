cmake_minimum_required(VERSION 3.16)
project(pc_sim C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ---------------------------------------------------------------- LVGL
# 直接复用工程里那份 LVGL（与板子同版本），只换配置文件
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../components/lvgl"
                 "${CMAKE_CURRENT_BINARY_DIR}/lvgl_build")

# ---------------------------------------------------------------- SDL2
find_package(SDL2 REQUIRED)

# ---------------------------------------------------------------- 界面源码
# 只编"纯 LVGL"的界面文件：
#   lvgl_port.c  —— 依赖真实 LCD/触摸，PC 上用 stubs/sim_impl.c 替代
#   ui_manager.c —— 依赖 touch_service/ui_draw/FreeRTOS 任务，PC 上由 main_sim.c 替代
#   scene_splash.c —— 启动流程，模拟器不需要
# ★ 显式列出：只编真正要预览的 4 个页面 + 状态栏 + 日志缓冲。
#   不编 lvgl_port.c（真实 LCD）、ui_manager.c（FreeRTOS 调度）、
#   scene_splash.c / scene_* 测试画面（依赖 lcd.h / touch.h）。
set(UI_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/src/scene_home.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/src/scene_monitor.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/src/scene_audio.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/src/scene_system.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/src/ui_statusbar.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/src/ui_logbuf.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/src/ui_anim.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/src/ui_theme.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/src/scene_weather.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/src/scene_hf.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/src/scene_ai_rank.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/src/scene_bili.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/src/scene_voice.c"
)

file(GLOB FONT_SRC CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/src/fonts/*.c")

# ---- LVGL 官方 demos（PC 平台的 cmake 不自动包含，手动加）----
set(LVGL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../components/lvgl")
file(GLOB DEMO_SRC
    "${LVGL_ROOT}/demos/lv_demos.c"
    "${LVGL_ROOT}/demos/music/*.c"
    "${LVGL_ROOT}/demos/music/assets/*.c")

add_executable(pc_sim
    main_sim.c
    stubs/sim_impl.c
    ${UI_SRC}
    ${FONT_SRC}
    ${DEMO_SRC}
)

target_include_directories(pc_sim PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/BSP/AUDIO/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/BSP/STORAGE/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/BSP/LCD"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/BSP/LED/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/netapp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/voice/include"
    "${LVGL_ROOT}/demos"
)

target_link_libraries(pc_sim PRIVATE lvgl SDL2::SDL2main SDL2::SDL2 m)

# 板上这些值来自 sdkconfig，PC 上给固定值（与 sdkconfig 一致）
target_compile_definitions(pc_sim PRIVATE
    CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=320
    CONFIG_IDF_TARGET="esp32s31"
    PC_SIM=1
)

# MinGW 缺 localtime_r 等 POSIX 函数：用兼容头强制注入（只作用于本 target）
target_compile_options(pc_sim PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/stubs/sim_compat.h")

# Windows 下把 SDL2.dll 拷到 exe 旁边，双击就能跑
if(WIN32)
    add_custom_command(TARGET pc_sim POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "C:/msys64/mingw64/bin/SDL2.dll"
                "$<TARGET_FILE_DIR:pc_sim>"
        COMMENT "copy SDL2.dll")
endif()
