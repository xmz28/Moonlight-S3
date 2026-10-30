# 性能优化实测 — 2026-10-30

硬件：ESP32-S3 rev0.2、16 MB Flash、8 MB OPI PSRAM、800×480 RGB565、GT911。
Windows 10，Intel Iris Xe，实际桌面2560×1440；用户现有2.4 GHz电脑热点。
每档均采用JPEG quality60、baseline 4:2:0、目标25 FPS。原始和中间版本每档25秒，最终版每档30秒。
所有表中数据均为真实桌面串流及LCD完成确认，不使用合成图或循环发送旧帧。
桌面内容与系统负载会变化，以下是该设备/该网络的阶段记录，不是完全相同录像的受控对比。

## 端到端结果

| 阶段 | 400×240 FPS | 800×480 FPS |
|---|---:|---:|
| 原始版本 | 6.03 | 5.07 |
| DXGI 主机 + 原固件 | 12.04 | 11.23 |
| 分块解码 + 三缓冲 / 两帧在途 | 17.14 | 9.63 |
| 最终流畅优先 / 三帧在途 | 22.71 | 11.69 |

最终版详细结果：

- 400x240：682 个LCD确认 / 30.032秒，平均22.709 FPS；错误0，会话异常None。
  电脑端每帧平均抓屏/缩放 8.41 ms，叠加/JPEG编码 0.51 ms，发送调用 0.09 ms；平均JPEG 14.1 KiB。
  每秒报告采样的帧确认耗时 93–219 ms，采样均值 129.4 ms。该数据不是触摸到画面的总延迟。
- 800x480：351 个LCD确认 / 30.031秒，平均11.688 FPS；错误0，会话异常None。
  电脑端每帧平均抓屏/缩放 38.17 ms，叠加/JPEG编码 3.35 ms，发送调用 1.60 ms；平均JPEG 34.5 KiB。
  每秒报告采样的帧确认耗时 141–375 ms，采样均值 209.9 ms。该数据不是触摸到画面的总延迟。

停止时仍在途的最多三帧不计作丢帧。GUI短窗口FPS会波动，最终平均值计入握手及起始空档。
400×240输出在S3上按最近邻放大至800×480，清晰度与800×480原生档不同。
最终400×240采样多在20–25 FPS，但不保证任意桌面/网络/负载都达到此范围。
800×480尚未达到20 FPS。

## 改动及边界

- 主机优先DXGI桌面复制，直接映射BGRA缓冲，缩小后才转换颜色；避免完整2560×1440画面的Python RGB重排。
- 编码直接在400×240或800×480画布上合成，不再先画800×480再缩小整帧；工具栏按模式缓存。
- DXGI按完整显示器边界匹配，支持负坐标；不能匹配或初始化失败时依次回退GDI和MSS。
- DXcam固定0.3.0，针对其COM释放的实例适配保存在capture.py。已验证反复创建/释放和多个GUI工作线程；未实际切换显示器拓扑。
- S3使用JPEG分块解码，25,600-byte内部SRAM块；400×240在内部SRAM展开像素行，连续拷贝到PSRAM；800×480直接拷贝块。
- S3使用三个RGB帧缓冲。core1负责解码，core0提交LCD，等待LCD时可并行准备下一帧。只有真实LCD完成回调后才回收旧缓冲并发送ACK。
- LCD提交与旧回调信号清理在同一临界区中，避免旧回调误判新提交完成；网络状态页要求JPEG和RGB缓冲都空闲。
- 触摸每10ms轮询，变化及时发送；按住不动20ms心跳、空闲50ms心跳，Windows失联松键阈值仍350ms。
- 默认“流畅优先”允许三帧在途，防止网络和渲染管线空转；关闭后最多两帧。两者都有严格上限，不持续积累旧截图。
- RGB565接线、12MHz PCLK、800×480逻辑触摸坐标保持一致。没有通过提高屏幕时钟取得上述结果。

## 验证

- 最终固件已使用共享C盘工具链编译并烧录COM17，写入哈希校验通过。
- 23项主机测试通过：包括两/三帧流控、报文拆分、CRC、心跳松键、断线松键、坐标/颜色和抓屏资源释放。
- 实机故障恢复12项通过：3次坏CRC、3次坏JPEG均返回ERR1；随后6次400/800交替帧均成功ACK。
- 最终EXE打包后验证GUI和实际DXGI抓屏/JPEG编码。
- 用户确认优化版“画面正常”，并确认“触摸和画面都正常”；控制回归90秒，记录281条触摸事件、16次按下起点、1804帧确认、零会话异常；LEFT/RIGHT模式均有记录。数据见performance/control-regression.json。
- 尚未验证长期运行、所有屏幕角点、全部滚轮/键盘操作、显示器拓扑变化或差网络丢包环境。

## 复测

在电脑配套项目中执行（使用现有Python环境，不新建venv）：

```powershell
python tools/benchmark_stream.py --serial COM17 --seconds 30 --output build/benchmark.json
python tools/benchmark_stream.py --serial COM17 --seconds 30 --low-latency --output build/benchmark-low-latency.json
```

两个基准命令均关闭电脑输入控制，只投屏，不保存截图，不输出Wi-Fi密码或配对码。
串口参数可省略。普通同一局域网也可运行；本次热点由用户提供，S3始终STA模式。
原始记录位于本目录的performance/，最终记录为optimized-smooth.json。

实现参考：[DXcam官方项目](https://github.com/ra1nty/DXcam)、[乐鑫分块解码示例](https://github.com/espressif/esp-adf-libs/blob/master/esp_new_jpeg/test_app/main/test_decoder.c)、[乐鑫RGB驱动](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_lcd/rgb/esp_lcd_panel_rgb.c)。
