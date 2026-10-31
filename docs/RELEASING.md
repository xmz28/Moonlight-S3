# 双仓库与发布说明

当前采用两个独立公开源码仓库：[S3 固件](https://github.com/xmz28/Moonlight-S3)、[Windows 端](https://github.com/xmz28/Moonlight-S3-Host)。协议以固件仓库的 [PROTOCOL.md](PROTOCOL.md) 和 [PAIRING.md](PAIRING.md) 为准，电脑端发布时链接到配套固件 tag。

## 版本对应

每次 Release 记录本仓库源码提交、配套仓库 tag/提交、目标硬件、协议版本和验证范围。当前协议为 MS3 v3 会话控制 / v2 PIN 授权 / v1 JPEG 帧格式，不能仅以 JPEG 帧版本判断配对兼容性。两端独立迭代，影响协议时一起验证并更新兼容表。

2026-10-31 当前源码基线：固件 `5dfb457`、电脑端 `f2bbff2`。这些是历史开发提交，不是已发布版本。用户已确认本轮修改并授权提交 GitHub；本次上传源码，正式 Release 及二进制附件另行发布。

## 固件构建和配置

- 默认 PlatformIO 使用开发者自己的 core；本机通过忽略的 `platformio.local.ini` 继续复用已有共享环境。
- Wi-Fi 配置可缺省；`python tools/prepare_local.py` 仅在缺失时创建空配置，不覆盖已有文件。
- `python tools/verify_artifacts.py` 独立校验芯片、Flash、分区边界、组合镜像和字库内容。可用 `--host-dir <电脑端目录>` 额外校验存在的本机授权配置；不输出凭据。
- 公开固件必须从空 Wi-Fi 默认配置构建，避免本机 SSID/密码被编译到应用；Git 忽略配置文件不等于构建产物不含配置。使用单独干净检出进行发布构建，保留日常开发目录的 Wi-Fi 配置。

## 固件附件

首次安装包提供完整 `firmware.factory.bin`，从偏移 0 写入，包含 bootloader、分区表、应用和中文字体。当前新增 `ms3_store`（0x910000，64 KB）配置分区。从旧布局迁移必须同步更新分区表（0x8000）和应用（0x10000），使用标准 PlatformIO upload，保留旧 NVS，首次启动自动迁移本项目配置。确认分区布局相同后，才可只更新应用及必要的 `ui_font.bin`（0x610000）。完整组合镜像会覆盖旧 NVS，不用于保留配置的迁移。字体不能遗漏。日常源码更新优先使用 PlatformIO upload。

附 `INSTALL.zh-CN.md`、`SOURCE.txt`、`SHA256SUMS.txt`，说明 ESP32-S3-N16R8、16 MB Flash、8 MB OPI PSRAM、RGB565/GT911 引脚要求，以及每个文件的偏移。首次组合镜像会覆盖 NVS 区域，必须与保留配置的更新方式区分。不要把整机 Flash dump 当公开固件，它可能含 NVS 中的 Wi-Fi 和授权数据。

## Windows 附件

电脑端 `build.ps1` 输出到独立 `dist-release/`，保留原 `dist/` 开发软件及其配置。ZIP 包含 exe、完整 `_internal/`、README、第三方声明、licenses 和源码信息，外附 SHA-256；不复制本机 `settings.json`。解压到可写目录，配置固定保存在 `%LOCALAPPDATA%\MoonlightS3Host\settings.json`；首次无配置才生成身份。首次迁移程序旁旧配置并保留原文件，更新目录不改变身份；升级前从托盘退出程序并保留固定配置目录。

## 发布前检查

检查两个仓库暂存内容、Git 历史和实际 ZIP/固件附件中的配置；运行各自构建和验证，按本轮实际范围记录软件测试与实机结果。已有实机性能记录是历史证据，不作为每次整理后的新实测。

目前两边自有代码尚未选定顶层 LICENSE；保留第三方许可证，自有代码许可后续单独确认，不直接沿用参考项目的 GPL/AGPL。用户已授权本次提交并上传公开源码；后续二进制 Release 单独制作和验证。
