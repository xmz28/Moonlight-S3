# Moonlight S3 — ESP32-S3 触屏 JPEG 云桌面

本项目包含 ESP32-S3 接收端；配套 Windows 发送端位于：
`C:\Users\10145294\Documents\MySoftware\Moonlight_S3_Host`。

电脑抓取指定显示器 → 缩放并编码 baseline JPEG 4:2:0 → Wi-Fi TCP →
ESP32-S3 的 SIMD JPEG 软件解码 → RGB565 屏幕。
GT911 触摸通过同一连接回传，由电脑调用 Windows SendInput。

这是自定义 JPEG 串流协议，不兼容 Moonlight/Sunshine 的 GameStream 协议。
首版支持 Windows 桌面、左键点击/拖动、右键、滚轮和 Windows 屏幕键盘入口。
没有音频、剪贴板、文件传输、原生多点 Windows 触摸、互联网中继和 TLS。
优先用于本地 Wi-Fi；远程使用需要自行提供可路由的 VPN，不能直接连接公网云电脑服务商协议。

## 硬件与接线

以现有 **LVGL-S3** 的 `src/main.cpp` 为依据，默认硬件为 **ESP32-S3-N16R8**：
240 MHz 双核，16 MB Flash，8 MB OPI PSRAM，800×480 并行 RGB565，GT911。
已通过 COM17 实测并烧录 ESP32-S3 rev 0.2、16 MB Flash、8 MB PSRAM，GT911 地址 0x5D。

| 信号 | GPIO |
|---|---|
| RGB D0–D15 | 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21 |
| PCLK / HSYNC / VSYNC / DE | 1 / 39 / 40 / 41 |
| 背光逻辑使能 | 42，高电平 |
| GT911 SDA / SCL / INT | 47 / 48 / 2 |
| GT911 地址 | 自动检测 0x5D、0x14 |

GT911 复位脚参考工程未配置，本项目不猜测额外接线。GPIO42 为背光逻辑使能，不能直接供给 LED 功率电源。
触摸方向在 `include/config.h` 中设置 swap/invert。
RGB 时序沿用 LVGL-S3 Wi-Fi 档：12 MHz PCLK，水平 48/40/8，垂直 2/40/6，负沿采样。
理论扫描频率约 `12000000 / (896 * 528) = 25.36 Hz`。

## 第一次运行

1. 烧录此固件后，S3 只以 STA 模式连接局域网 Wi-Fi，**任何情况下都不创建热点**。
2. 打开电脑端 `dist\MoonlightS3Host\MoonlightS3Host.exe`，点击“首次配置 S3 Wi-Fi（USB）”，
   选择 S3 的串口并填写路由器的 **2.4 GHz** Wi-Fi。工具先识别固件，再把配置保存到 NVS；
   保存后 S3 重启，之后独立联网，无需保持 USB 连接。这一步不会烧录固件。
3. 电脑通过网线或 Wi-Fi 接入同一局域网。点击“搜索局域网”，从列表明确选择设备。
   IP 和 TCP 端口自动填入；本机配对码已配置在两端，开始串流时验证。
4. 选择显示器，初始用 **400×240 / 25 FPS / 质量60**，点“开始串流”。
5. 在屏幕底部选择 LEFT / RIGHT / SCROLL / KEYBOARD，再在桌面区域操作。
   LEFT/RIGHT 模式按住移动会拖动；SCROLL 模式上下划动发滚轮。
   KEYBOARD 在 Windows 上启动系统屏幕键盘，其启动成功与否取决于系统组件及权限。
6. 电脑端“停止 / 释放鼠标”停止连接。断线或触摸心跳消失会自动释放按键。

也可以在本地忽略文件 `include/config_secrets.h` 中定义 `MOONLIGHT_WIFI_SSID` 和
`MOONLIGHT_WIFI_PASSWORD`，再编译；板内通过 USB 保存的配置优先。
未配置时停留在等待配网页面；连接失败每15秒重试，并保留 USB 配网入口，不回退热点。
DHCP 地址变化会重新建立 TCP 监听与发现服务，空闲屏幕显示新IP；电脑端重新搜索即可。
没有搜索结果时，可以手动填写屏幕上的IP，或 `moonlight-s3-xxxxxx.local`（依赖电脑的mDNS支持）。
局域网广播使用 UDP47990，串流使用 TCP47989；跨VLAN、访客网络客户端隔离或防火墙拦截可能阻止发现。
电脑可连接路由器的5GHz Wi-Fi或有线网络，只需与S3的2.4GHz网络互通。

配对码在 `include/config_secrets.h` 和电脑端 `settings.json`，这些文件不纳入 Git。
重建时可以运行 `python tools\prepare_local.py` 生成缺失配置；不会覆盖已有配对码。
已有电脑配置与固件不匹配时，手动同步配对码。连接使用明文应用协议，只在可信网络内使用。
程序不自动修改防火墙。发现使用广播查询和单播回复，电脑是串流的主动连接方。
USB配网密码不回显、不写入电脑设置文件；NVS存储未开启Flash加密。打开串口时某些USB驱动可能产生复位脉冲。

## 画质、帧率和延迟

| 档位 | 接收端处理 | 用途 |
|---|---|---|
| 800×480 | JPEG 分块解码到内部 SRAM，再拷贝到背面 RGB 缓冲 | 字体清晰优先 |
| 400×240 | 内部 SRAM 分块解码，2 倍最近邻放大 | 流畅优先，争取 20–25 FPS |

已完成投屏和触摸反控实测。初始版本为约6/5 FPS；2026-10-30优化后400×240平均22.71 FPS、800×480平均11.69 FPS，均零解码错误。分阶段记录及适用范围见 `docs/PERFORMANCE.md`。
乐鑫 esp_new_jpeg 1.0.2 公布的 S3、RGB565_LE、quality60、4:2:0 的 800×480
纯解码参考值为 16.87 FPS；网络、PSRAM、RGB 扫描还会影响端到端结果。
400×240 档减少解码像素，但有放大开销。当前使用三个 RGB 帧缓冲，让 core 1 解码/放大与 core 0 屏幕提交等待重叠。
像素块使用 25,600 bytes 内部 SRAM，整帧像素只在准备完成时连续写入 PSRAM。

电脑端显示的是帧确认统计：ACK 在解码完成且 LCD framebuffer 可复用回调之后发送。
解码耗时包含 JPEG 头解析；“显示处理”还包括放大/提交/同步等待；
“帧确认”从发送前到收到 ACK，不能当作触摸到画面的总延迟。
30 FPS 设置不会让 12 MHz 时序的面板超过约25 Hz。

默认流畅优先最多三帧在途；关闭该选项后最多两帧。电脑只有拿到空位才抓取最新屏幕，不累积截图队列。
TCP 在差网络下仍有重传排队；超时 3 秒断开，不承诺固定低延迟。
固件网络任务在 core0，解码任务在 core1。两个 256 KiB JPEG 缓冲、两个 RGB 缓冲和小图缓冲均使用 PSRAM。
LCD 同步异常时停止复用缓冲并返回错误，需重启排查，避免继续破坏扫描数据。

## 编译与更新

共用 **C:\pio-sm-s3** 的 platform / packages，不新建独立环境。
此机器该目录自身的 pio 启动器报 `uv trampoline failed to canonicalize script path`，
因此使用已存在的全局启动器，项目 `core_dir` 仍指向同一个共享环境：

```powershell
& 'C:\Users\10145294\.platformio\penv\Scripts\pio.exe' run -e moonlight_s3
```

工具链：pioarduino 55.03.311 / Arduino 3.3.11 / ESP-IDF libs 5.5.5。
项目独立板定义 `boards/moonlight-s3-n16r8.json` 明确锁定 ESP32-S3、16 MB Flash 和 OPI PSRAM。
镜像 Flash header 沿用参考工程的 DIO，内部 memory_type=qio_opi。
分区使用单 factory 应用和独立 ui_font 字库数据分区；未提供 OTA。

输出：

- `.pio\build\moonlight_s3\firmware.bin`：应用程序，偏移 0x10000。
- `.pio\build\moonlight_s3\firmware.factory.bin`：含 bootloader、分区表、应用和字库的完整组合镜像，偏移 0。
- `.pio\build\moonlight_s3\firmware.elf`：调试符号。

更新设备：`pio run -e moonlight_s3 -t upload --upload-port COM17`（使用上述已有启动器）。正常上传写入应用、分区表及字库，不擦除 NVS。完整 factory 镜像仅用于首次安装；整镜像从偏移0写入会覆盖中间 NVS 区域，已有配网更新请使用正常 upload。

## 工程来源和验证范围

- LVGL-S3：硬件引脚、RGB 时序、GT911 寄存器、esp_new_jpeg 1.0.2 S3 归档库。
- bleconnect 的 `YadeaOtaClient.sendImagePackets`：参考 JPEG 分帧与 CRC 思路。
  本项目不复用车辆握手、固定车辆身份、BLE 或 Android UDP 8889 报文。
- 新协议详见 `docs/PROTOCOL.md`。
- 主机测试涵盖 wire layout、CRC、JPEG 格式、TCP 拆包、两/三帧流控、坐标映射、工具栏和失联松键。
  使用 loopback 模拟接收端和虚拟鼠标，不代表真实 S3 解码或实际 Windows 点击已经验证。
- 已烧录 ESP32-S3 实机；用户确认画面、触摸、中文、Wi-Fi 扫描连接以及开始/返回操作正常。帧率受网络与桌面内容影响；端到端触摸延迟和长期稳定性尚未完整测量。

参考：
- https://components.espressif.com/components/espressif/esp_new_jpeg/versions/1.0.2/readme
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-mouseinput
- https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html
- https://pyserial.readthedocs.io/en/latest/pyserial_api.html


## 屏上首页与 Wi-Fi 设置（2026-10-30）

首页显示中文状态、当前网络和 IP。电脑配套软件保持打开并选好接收端、配对码及画面参数，然后在 S3 上点“开始串流”；约一秒内由电脑发起连接。找不到软件时请求15秒后超时，可重试。也可直接在电脑点开始。

串流底栏依次为 LEFT、RIGHT、SCROLL、KEYBOARD、BACK；BACK 停止当前串流并返回 S3 首页。仅投屏模式下 BACK 仍可用；隐藏工具栏时从电脑停止。

首页右上角“Wi-Fi 设置”支持扫描、翻页、选择网络、输入密码和连接。连接成功并取得地址后保存到 NVS，失败不覆盖旧凭据；退出设置可恢复旧网络。支持2.4GHz普通个人/开放网络，暂不支持企业认证。键盘输入 ASCII；中文网络名可从扫描列表选择。

扫描仅在设置页手动触发，串流期间不扫描、不绘制设置页。字体按字形从独立 Flash 分区读取，复用已有 RGB 帧缓冲，不把整套中文字体放入 PSRAM。详细验证及性能限制见 [WIFI_UI.md](docs/WIFI_UI.md)。
