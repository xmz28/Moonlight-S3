# 验证记录 — 2026-10-29

## 已通过

- ESP32-S3-N16R8 PlatformIO release 构建成功。
- 编译依赖共用 `C:\pio-sm-s3`；启动器为已有的
  `C:\Users\10145294\.platformio\penv\Scripts\pio.exe`。
- 固件静态 RAM 48,328 bytes；应用 bin 1,111,392 bytes，位于6 MiB应用分区内。
- 完整组合镜像 1,176,928 bytes。
- 离线检查了应用和 bootloader 的 ESP32-S3 chip ID=9、DIO、16MB header。
- 检查了分区边界以及组合镜像中 bootloader、分区表、应用内容的一致性。
- 固件、主机源码和打包软件的配对码相同（验证过程不输出配对码）。
- 主机12项 unittest 测试全部通过；包含 TCP loopback 接收端模拟、报文拆分、
  CRC、两帧流控、JPEG基线/4:2:0检查、坐标边界、工具栏、失联松键及 Win32 INPUT ABI。
- Windows PyInstaller onedir 打包成功。
- 最终 EXE 的 `--gui-smoke` 退出码为0：创建完整GUI，并完成400×240和800×480的合成图JPEG编码/解码。

哈希及分区详情见 `.pio/build/moonlight_s3/artifact-manifest.json`。
重复离线镜像检查：`python tools/verify_artifacts.py`。

## 尚未验证

没有执行烧录、擦除或打开硬件串口。
没有真实S3解码、Wi-Fi串流、面板显示/颜色/撕裂、GT911轴向/持续按压、Windows真实输入注入、
系统屏幕键盘启动或云主机/VPN环境测试。
软件测试使用合成图与虚拟鼠标，因此不能据此宣称实际20–25FPS或特定触摸延迟。

## 实机验证顺序

1. 以后获得烧录授权后，核对实际板卡N16R8、接线与供电，使用本项目组合镜像。
2. 检查串口中的PSRAM容量、GT911地址、IP，以及屏幕启动文字。
3. 先以400×240、25FPS、quality60投屏，观察帧确认FPS和解码耗时。
4. 在非关键测试窗口核对四角、单击、双击、拖动、右键、滚轮、工具栏和失联松键。
5. 切换800×480对比清晰度、解码耗时、Wi-Fi吞吐与屏幕稳定性，再决定性能优化方向。
