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
新项目没有实际读到设备，以上是参考工程配置；烧录前核对是否仍是同一块板。

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

## 第一次运行（本次尚未烧录）

1. 未来烧录此固件后，S3 默认建立 `Moonlight-S3` 热点，密码 `MoonlightS3`。
2. 电脑连接该热点，打开电脑端 `dist\MoonlightS3Host\MoonlightS3Host.exe`。
3. 默认 IP `192.168.4.1`，端口 `47989`；本机生成的配对码已经写入两端配置。
4. 选择显示器，初始用 **400×240 / 25 FPS / 质量60**，点“开始串流”。
5. 在屏幕底部选择 LEFT / RIGHT / SCROLL / KEYBOARD，再在桌面区域操作。
   LEFT/RIGHT 模式按住移动会拖动；SCROLL 模式上下划动发滚轮。
   KEYBOARD 在 Windows 上启动系统屏幕键盘，其启动成功与否取决于系统组件及权限。
6. 电脑端“停止 / 释放鼠标”停止连接。断线或触摸心跳消失会自动释放按键。

也可以在 `include/config.h` 中填写 `WifiSsid`、`WifiPassword`，让 S3 加入现有 **2.4 GHz** 路由器。
开机 15 秒内连接失败则退回热点。成功后的 IP 显示在屏幕和串口上，在电脑端填写这个 IP。
STA 模式自动重连；如果 DHCP 地址发生变化，以路由器租约为准，建议地址保留。
如果电脑要同时上互联网，可用以太网联网或采用 STA 模式。

配对码在 `include/config_secrets.h` 和电脑端 `settings.json`，这些文件不纳入 Git。
重建时可以运行 `python tools\prepare_local.py` 生成缺失配置；不会覆盖已有配对码。
已有电脑配置与固件不匹配时，手动同步配对码。连接使用明文应用协议，只在可信网络内使用。
首次运行不自动开防火墙规则，电脑是主动连接方。

## 画质、帧率和延迟

| 档位 | 接收端处理 | 用途 |
|---|---|---|
| 800×480 | JPEG 原生解码到背面 RGB 缓冲 | 字体清晰优先 |
| 400×240 | JPEG 解码到小缓冲，2 倍最近邻放大 | 流畅优先，争取 20–25 FPS |

**未进行本项目实机帧率测试，20 多帧是目标，不是已验证结果。**
乐鑫 esp_new_jpeg 1.0.2 公布的 S3、RGB565_LE、quality60、4:2:0 的 800×480
纯解码参考值为 16.87 FPS；网络、PSRAM、RGB 扫描还会影响端到端结果。
400×240 档减少解码像素，但有放大开销，需实机测量。

电脑端显示的是帧确认统计：ACK 在解码完成且 LCD framebuffer 可复用回调之后发送。
解码耗时包含 JPEG 头解析；“显示处理”还包括放大/提交/同步等待；
“帧确认”从发送前到收到 ACK，不能当作触摸到画面的总延迟。
30 FPS 设置不会让 12 MHz 时序的面板超过约25 Hz。

最多两帧在途，电脑只有拿到空位才抓取最新屏幕，不累积截图队列。
TCP 在差网络下仍有重传排队；超时 3 秒断开，不承诺固定低延迟。
固件网络任务在 core0，解码任务在 core1。两个 256 KiB JPEG 缓冲、两个 RGB 缓冲和小图缓冲均使用 PSRAM。
LCD 同步异常时停止复用缓冲并返回错误，需重启排查，避免继续破坏扫描数据。

## 编译（只编译，不烧录）

共用 **C:\pio-sm-s3** 的 platform / packages，不新建独立环境。
此机器该目录自身的 pio 启动器报 `uv trampoline failed to canonicalize script path`，
因此使用已存在的全局启动器，项目 `core_dir` 仍指向同一个共享环境：

```powershell
& 'C:\Users\10145294\.platformio\penv\Scripts\pio.exe' run -e moonlight_s3
```

工具链：pioarduino 55.03.311 / Arduino 3.3.11 / ESP-IDF libs 5.5.5。
项目独立板定义 `boards/moonlight-s3-n16r8.json` 明确锁定 ESP32-S3、16 MB Flash 和 OPI PSRAM。
镜像 Flash header 沿用参考工程的 DIO，内部 memory_type=qio_opi。
分区使用单 factory 应用；首版未提供 OTA，不要套用双 OTA 分区升级方式。

输出：

- `.pio\build\moonlight_s3\firmware.bin`：应用程序，偏移 0x10000。
- `.pio\build\moonlight_s3\firmware.factory.bin`：含 bootloader、分区表的完整组合镜像，偏移 0。
- `.pio\build\moonlight_s3\firmware.elf`：调试符号。

**本轮只生成镜像，不执行 upload、擦除或访问串口设备。**

## 工程来源和验证范围

- LVGL-S3：硬件引脚、RGB 时序、GT911 寄存器、esp_new_jpeg 1.0.2 S3 归档库。
- bleconnect 的 `YadeaOtaClient.sendImagePackets`：参考 JPEG 分帧与 CRC 思路。
  本项目不复用车辆握手、固定车辆身份、BLE 或 Android UDP 8889 报文。
- 新协议详见 `docs/PROTOCOL.md`。
- 主机测试涵盖 wire layout、CRC、JPEG 格式、TCP 拆包、两帧流控、坐标映射、工具栏和失联松键。
  使用 loopback 模拟接收端和虚拟鼠标，不代表真实 S3 解码或实际 Windows 点击已经验证。
- 未烧录，未验证屏幕颜色/撕裂、触摸轴向、Wi-Fi 吞吐、实际 FPS 或端到端延迟。

参考：
- https://components.espressif.com/components/espressif/esp_new_jpeg/versions/1.0.2/readme
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-mouseinput
