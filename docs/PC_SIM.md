# PC 模拟器（pc_sim）

> 目的：**改界面不用烧板子**。把 `components/ui/` 的界面代码原样编成 Windows 程序，
> 用 SDL 开一个 800×480 窗口，鼠标当触摸。调像素、试配色、改布局全程在这里完成，
> 满意了再烧板子。

## 启动

```
双击 pc_sim\run.bat          ← 自动 配置+编译+运行（改完代码用它，约 5 秒）
双击 pc_sim\build\pc_sim.exe ← 直接跑已编译的版本（秒开）
```

窗口内：

| 操作 | 作用 |
|---|---|
| 鼠标点击 / 拖动 | 等同触摸屏 |
| **1 / 2 / 3 / 4** | HOME / PC MONITOR / AUDIO / SYSTEM |
| **Esc** | 退出 |
| 命令行 `--scene N` | 启动直接进指定页（0~3），便于逐页截图检查 |

## 工程结构

```
pc_sim/
  CMakeLists.txt     链接 LVGL（复用工程里那份）+ SDL2
  lv_conf.h          PC 侧 LVGL 配置（RGB565、LV_OS_NONE、字体 14/20/24/28）
  main_sim.c         SDL 窗口 + 鼠标→触摸 + 场景循环
  run.bat            一键编译运行（★ 必须纯 ASCII：cmd 按 GBK 解析 bat）
  stubs/
    esp_*.h          ESP-IDF 头的最小替身（日志/时间/芯片/堆/FreeRTOS）
    sim_compat.h     MinGW 缺的 localtime_r（用 -include 强制注入）
    sim_impl.c       板级模块的假实现：音频/PC监控/WiFi/SD/移植层
```

**编译哪些界面文件**：`scene_home/monitor/audio/system.c`、`ui_statusbar.c`、`ui_logbuf.c`、`fonts/*.c`
——**一行不改**。

**不编译**：`lvgl_port.c`（真实 LCD/触摸）、`ui_manager.c`（FreeRTOS 调度）、
`scene_splash.c` 与其它测试画面（依赖 `lcd.h`/`touch.h`）——这些由 `main_sim.c` + stub 替代。

## 加一个新页面的步骤

1. 在 `components/ui/src/` 写 `scene_xxx.c`（照 `scene_system.c` 的结构）；
2. `ui_scenes.h` 加枚举和声明；
3. `pc_sim/CMakeLists.txt` 的 `UI_SRC` 列表里加一行；
4. `main_sim.c` 的 switch 里加一个 case + 一个数字键；
5. 重新编译即可预览——**板子那边随后再补 ui_manager 的场景表**。

## 能验证 / 不能验证

| 能 | 不能 |
|---|---|
| 布局、间距、字号、颜色 | **边缘闪烁**（PSRAM/DMA 带宽问题） |
| 控件位置、遮挡、溢出 | 音频真实出声、音质 |
| 交互逻辑（点击/切换/状态） | 触摸手感、真实刷新率 |
| 长文本截断/省略 | SD 卡真实读速、WiFi 真实信号 |

## 已知限制（模拟器特有，不是 BUG）

- **数据是假的**：CPU/GPU 上下跳、RSSI 在 -45~-88 之间摆动、SD 曲目固定 3 首；
- **PSRAM 显示 210KB/320KB**（板子上是 16MB）——stub 的 `heap_caps_*` 对所有 caps 返回同一个值；
- **SYSTEM 页日志区为空**——`ui_logbuf` 依赖 `esp_log_set_vprintf`，PC 上是空实现。

## ★ 必须自己调的两个初始化

板子上 `ui_manager_run()` 会调 `ui_logbuf_init()` 和 `ui_statusbar_init()`；
模拟器不编 `ui_manager.c`，所以 `main_sim.c` 里**必须手工调这两个**，
否则状态栏（时间/WiFi）和日志缓冲都不会出现（第一次搭时踩过）。

## 截屏自查（给 AI 用）

```
python tools\screenshot.py out.bmp --window "ESP32-S31 UI"
```

纯 ctypes 调 GDI32，不依赖 Pillow。会**先把窗口置顶 0.4 秒**再截（否则只能截到遮挡物），
截完恢复。带 `--window` 时按标题片段找窗口，找不到退回全屏。
