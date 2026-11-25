# 交接文档（2026-11-25 21:45）

> 给下一个会话用。**读完这一页就能直接上手**，不需要翻历史。
> 项目：ESP32-S31 桌面屏（800×480 RGB + 语音助手 + 手机风 UI）
> 工程路径：`C:\Users\Admin\Desktop\sample_project`

---

## 0. 一分钟状态

| 项 | 值 |
|---|---|
| **分支** | `feature/mobile-ui`（6 个提交未推送） |
| **工作区** | 干净 |
| **设备** | `192.168.1.3`（COM5 烧录） |
| **设备上跑的固件** | 手机风图标网格 + **壁纸关闭**（`if (0 &&`） |
| **服务** | monitor 8080 / asr 8081 / sovits 9880，全在跑 |
| **voice 链路** | 端到端 **3.4 秒**，持续对话可用（长按 KEY0 1 秒） |

---

## 1. 今天做完的（都在 `feature/mobile-ui` 上）

| 提交 | 内容 |
|---|---|
| `9be52229` | VAD 阈值 300→220（适配轻声，含 NVS 运行时钳位） |
| `8f57f7a2` | SoVITS 预热改后台线程+重试（10s → 3.4s 的关键） |
| `945e8abd` `ee102526` `e610aad9` `c7011f6e` | 持续对话模式（长按 KEY0，播完自动再听） |
| `56b44b1b` | 重建 `tools/watch_log.py`（零扰动串口监听） |
| `4fd88cde` | 手机风图标网格（96×96 纯色 + 48px 图标 + 深色底） |
| `e305476c` | `tools/unpack_pkg.py`（解包 WE 的 scene.pkg） |
| `0407dbad` | `ui_wallpaper` 模块（设备端硬件 JPEG 解码） |
| `90a23af7` | 壁纸分流：APPS 固定 `desktop.jpg`，时钟页随机 `up_*.jpg` |
| `d10f687e` | **修渲染核心**：脏区同步改包围盒（修左上角残留+竖线） |

---

## 2. 待办（优先级从高到低）

### ★ 2.1 开壁纸（当前是关着的）

`components/ui/src/scene_home.c:389` 有这么一行：

```c
if (0 && !wallpaper_apply_named(scr, "desktop.jpg"))   // ← 把 0 && 去掉就开
```

**开启前必须先查一件事**：脏区记账是否溢出。

```c
// lvgl_port.c
#define MAX_DIRTY 16
if (s_dir_cur_n < MAX_DIRTY) { ... } else { s_dir_cur_ovf = true; }
// 溢出后 → cache_sync_rows(px_map, 0, LCD_V_RES-1) 整屏 768KB
```

**假设**：有壁纸时脏区块数容易 >16 → 退化成整屏 cache sync → PSRAM 带宽爆 → **四周频闪**。

**验证方法**：在 `flush_cb` 的 `is_last` 分支加一行日志打印 `s_dir_cur_n` 和 `s_dir_cur_ovf`，看是不是经常 OVF。
**如果是**：把 `MAX_DIRTY` 提到 32/48，或者溢出时只同步包围盒（不整屏）。

### 2.2 渲染预算（做 UI 优化时一起）

当前 APPS 页：`render 23ms / flush 8ms / 50 FPS`（理论 66）。
开销排序：**阴影 shadow_width=6 > 圆角 24 > 渐变背景 > flush memcpy**。
硬件 `PPA` 可用但未接（`CONFIG_SOC_PPA_SUPPORTED=y`）。

### 2.3 大时钟（已放弃，想再试看这里）

48px 时钟 + 每秒刷新 → render 23→45ms、FPS 50→39，**不划算**。
代码在提交 `6a9d2109`（reflog 里也能找到）。
**教训**：任何"每秒刷新的全屏大字体"都要先算渲染成本。

### 2.4 语音后续

- 流式 TTS（首字 3.4s → 1.2s）：要 PC + 设备两端同改，半天到一天
- 双核占用统计：卡在改 sdkconfig 触发全量重编译

---

## 3. 关键文件

| 文件 | 作用 |
|---|---|
| `components/ui/src/scene_home.c` | APPS 页（手机风图标网格 + 壁纸调用） |
| `components/ui/src/ui_wallpaper.c/h` | 壁纸加载（设备端硬件解码 / PC 端 stub） |
| `components/ui/src/lvgl_port.c` | **渲染核心**（脏区记账 + 双缓冲 sync）★ 慎改 |
| `components/ui/src/fonts/ui_font_hw48.c` | 48px Font Awesome（17 个字形） |
| `components/voice/src/voice.c` | 语音主流程（持续对话在这里） |
| `components/netapp/src/wifi_prov.c` | 配网页 + `/upload` `/wpcopy` `/wplist` `/wpdel` `/wpclear` |
| `tools/watch_log.py` | 零扰动串口监听（dtr/rts 在 open 前设） |
| `tools/unpack_pkg.py` | 解包 WE 的 scene.pkg |
| `pc_sim/` | PC 模拟器（`run.bat` 或 `build/pc_sim.exe --scene 4`） |
| `Temp/wp_800x480.jpg` | 从创意工坊提取的壁纸（3840×2160 原图在 `Temp/wp_original.jpg`） |

---

## 4. 今天踩的坑（别重踩）

### 4.1 字体的 fallback 陷阱
`ui_font_hw`（24px）的 range 里**没有** 0xF017/0xF130/0xF1DE，靠 fallback 凑出来的。
生成 48px 版时忘记补这三个码点 → **第三行三个图标变空心方框**。
**规律**：LVGL 字体缺字形时，有 fallback 就悄悄替换，没 fallback 就是 tofu 方框。

### 4.2 新增 .c 必须 reconfigure
`components/ui/CMakeLists.txt` 用 `SRC_DIRS "src" "src/fonts"`（glob），
新增 `ui_font_hw48.c`、`ui_wallpaper.c` 后**必须 `idf.py reconfigure`**，否则链接失败。

### 4.3 WE 的 scene.pkg 格式
- `PKGV0018`：`uint32 ver_len + "PKGV0018" + uint32 count`，每项 `name_len + name + offset + size`
- **offset 是相对"数据区起点"**（文件表结束处），不是绝对偏移。第一版按绝对读，`scene.json` 读出的是 pkg 自己的文件头
- `.tex` 里是 **TEXB0003 + FreeImageFormat=512(FIF_JPEG)**，**直接是 JPEG**，不是 DXT 压缩块
- `preview.gif` 只有 255×255，放大会糊

### 4.4 脏区记账的包围盒问题
LVGL 内部 `refr_sync_areas` 会把多个小脏区**合并成大矩形**再 CPU memcpy。
只按逐块记账 → 块间空隙从没 sync → DMA 读到两帧前的像素 → **残留 + 竖线**。
修法：记录**包围盒**（min/max），按盒子的 y 范围整行同步。

### 4.5 pc_sim 相关
- 不编译 `scene_clock.c`（依赖硬件 JPEG 解码）
- 壁纸模块用 `#ifdef ESP_PLATFORM` 分支，PC 端返回 false
- 编译前要关掉正在跑的 `pc_sim.exe`，否则 `Permission denied`
- 截图：`pc_sim.exe --scene 4 --shot xxx.bmp --shot-after 50`

---

## 5. 常用命令

```bash
# 编译烧录
cd /c/Users/Admin/Desktop/sample_project
. .\idf_env.ps1   # PowerShell
idf.py build
idf.py -p COM5 -b 1500000 flash

# 新增源文件后
idf.py reconfigure

# PC 模拟器
cd pc_sim && ./run.bat                    # 编译+运行
./build/pc_sim.exe --scene 4 --shot a.bmp --shot-after 50   # 截图

# 零扰动串口监听（不会复位设备！）
python tools/watch_log.py 40 "voice:" "sysmon" "wallpaper"

# 设备接口
curl -s http://192.168.1.3/wplist
curl -s -X POST http://192.168.1.3/upload -H "Content-Type: image/jpeg" --data-binary "@图片.jpg"
curl -s "http://192.168.1.3/wpcopy?f=up_3.jpg&t=desktop.jpg"
curl -s "http://192.168.1.3/wpdel?f=up_0.jpg"

# 生成图标字体
npx lv_font_conv --font Temp/fa-solid-900.ttf \
  --range 0xf2db,0xf538,0xf863,0xf0a0,0xf6ff,0xf1eb,0xf2c9,0xf001,0xf013,0xf7c2,0xf06d,0xf080,0xf144,0xf26c,0xf017,0xf130,0xf1de \
  --size 48 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h \
  --lv-font-name ui_font_hw48 -o components/ui/src/fonts/ui_font_hw48.c
```

---

## 6. 稳定回退点

```bash
git tag stable-voice-3.4s        # 语音版稳定点（无 UI 改造）
git reset --hard stable-voice-3.4s
```

分支：`main` = 稳定语音版；`feature/mobile-ui` = UI 实验。
