# Moonlight-S3 v0.1.0 免编译安装

配套版本：S3 固件 v0.1.0 + Windows 软件 v0.1.0。协议为 v3 会话控制 / v2 PIN 授权 / v1 JPEG。此包不含本机 Wi-Fi 密码或配对凭据，烧录后在屏幕上设置 Wi-Fi 并首次授权。

## 适用硬件

ESP32-S3-N16R8，16 MB Flash、8 MB OPI PSRAM，800×480 并行 RGB565 与 GT911；必须与源码 README 的全部引脚和时序一致。它不是所有 ESP32-S3 屏幕的通用固件。

## 准备烧录工具

Windows 可从[乐鑫官方工具页](https://www.espressif.com/en/support/download/other-tools)获取 Flash Download Tools。选择 ESP32-S3 和设备对应的串口，镜像 Flash 参数为 DIO / 80 MHz / 16 MB。连接失败时按板卡说明进入下载模式，关闭占用串口的监视器。烧录工具不随本包分发。

也可使用已有的 esptool，按下面文件及偏移写入；不需要编译源码。Windows PowerShell 可通过 `Get-FileHash <文件> -Algorithm SHA256` 核对 SHA256SUMS.txt。

## 首次安装或恢复出厂

下载 first-install 包。仅用于新设备，或明确要清空全部 Flash 配置的恢复安装；擦除会清除该板原有项目、Wi-Fi 和配对数据。需要保留本项目配置时必须改用 update 包。

1. 在烧录工具中执行整片擦除。
2. 添加唯一文件：`firmware.factory.bin`，偏移 `0x0`，完成烧录并校验。
3. 重启设备，在首页“Wi-Fi 设置”连接 2.4 GHz 路由器。

组合镜像包含引导程序、分区表、应用及中文字库。不要只刷应用来首次安装。

## 保留现有 Moonlight 配置更新

下载 update 包；仅适用于本项目既有设备、旧 NVS 位于0x9000且原字库/应用布局一致。不要执行整片擦除，不要写入 first-install 组合镜像。其他项目迁移到本项目按首次安装处理。

勾选下面三个文件，分别写入并校验：

| 文件 | 偏移 | 用途 |
| --- | --- | --- |
| partitions.bin | 0x8000 | 新分区表，必须更新 |
| firmware.bin | 0x10000 | 应用程序 |
| ui_font.bin | 0x610000 | 中文字库 |

不要写入旧 NVS 的0x9000..0xDFFF，也不要覆盖 ms3_store 的0x910000..0x91FFFF。新版在独立64 KB ms3_store分区保存配置；首次从旧布局升级会迁移本项目旧 Wi-Fi、授权和界面设置，保留旧 NVS，不清空其他项目数据。旧分区表仅配新应用不能工作。

## Windows 软件和首次连接

在[电脑端 Latest Release](https://github.com/xmz28/Moonlight-S3-Host/releases/latest)下载 Windows x64 ZIP，完整解压后运行 exe，保留 _internal 文件夹。电脑和 S3 接入可互通的局域网；S3 点选电脑，首次将屏上四位 PIN 输入电脑弹窗即可。之后保留授权自动连接。没有音频或互联网中继，JPEG/触控数据仍未加密，只用于可信网络。

## 验证范围

公开镜像从不带 config_secrets.h 的干净源码构建，使用空 Wi-Fi 默认值；校验芯片、Flash、分区、字库、组合镜像和哈希。软件回归及GUI启动检查与历史实机验收分别记录；本次发布二进制未重新烧录、未重跑标准性能或长期稳定性测试。

SOURCE.txt记录源码提交和配套版本；压缩包包含第三方声明及许可证。GitHub自动附带的Source code不是烧录包。
