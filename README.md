# phigros_nx_cn

> 在 Nintendo Switch 上运行 Phigros 国服 Android ARM64 版本的 Homebrew
> 兼容层 / Loader。

**当前适配版本：Phigros 国服 4.0.0**

`phigros_nx_cn` 是基于
[ChanseyIsTheBest/phigros_nx](https://github.com/ChanseyIsTheBest/phigros_nx)
继续开发的独立维护版本，主要面向 Phigros 国服，并针对 Nintendo Switch
进行了兼容性、稳定性和运行时适配。

本项目不提供 Phigros 本体或游戏资源。运行时需要用户自行从合法获得的
Android 版本中准备所需文件。

## 快速下载

国内玩家可通过 **NS头号玩家** 获取已整理好的版本：

https://nsthwj.cn/game/switch/136910

## 开发动态

项目开发进度、实机演示和版本更新会持续发布在：

- 哔哩哔哩：[愛してるアイ的个人空间](https://space.bilibili.com/527174448)
- 抖音：[6eone6 的抖音主页](https://v.douyin.com/YpGlMkzkYEA/)

欢迎关注后续开发进展。

自行编译、开发和研究请参阅下方说明及 GitHub Releases。
[English](#english)

------------------------------------------------------------------------

## 项目状态

目前主要适配：

-   Phigros 国服 4.0.0
-   Nintendo Switch / Atmosphère Homebrew 环境
-   Unity 2022.3 / IL2CPP ARM64 Android 版本

已经完成的主要工作：

-   [x] Phigros 国服 4.0.0 基础适配
-   [x] 国服 IL2CPP / GC 运行时适配
-   [x] Nintendo Switch 运行稳定性修复
-   [x] 60 Hz 时序调整
-   [x] BGM / OpenSL 音频输出适配
-   [x] GC stop-the-world 稳定性修复
-   [x] 高物量谱面运行性能优化
-   [x] Android / JNI 兼容层调整
-   [x] 触摸与输入适配
-   [x] Android → Switch 存档迁移验证

当前版本已经完成实机稳定性测试，可正常进入并游玩大部分游戏内容。

不同主机、性能/超频设置和音频输出环境可能存在不同程度的谱面/音频延迟，请使用游戏内的延迟设置自行校准。
### 已知问题：剧情 CG / VideoPlayer

剧情 CG / Unity VideoPlayer 暂不可用。

部分主线内容会强制播放剧情视频。当前 Android MediaCodec / Unity
VideoPlayer
视频解码后端尚未完成，因此进入这些视频时可能出现黑屏或无法继续的问题。

计划实现基于 **FFmpeg** 的 Nintendo Switch 原生视频播放后端，以替代
Android MediaCodec 相关功能。

------------------------------------------------------------------------

## 安装与运行

你需要从自己合法获得的 **Phigros 国服 4.0.0 Android
版本**中提取运行所需文件。

示例目录：

``` text
sdmc:/switch/phigros/
├── phigros_nx.nro
├── libmain.so
├── libunity.so
├── libil2cpp.so
├── libnativeaudioe7.so
└── assets/
    ├── bin/Data/
    └── aa/
```

游戏资源需要从对应 Android 安装包 / Asset Pack 中自行提取。

本仓库不提供 APK、游戏资源、`libil2cpp.so`、`libunity.so`、`libmain.so`
或其他来自 Phigros 官方发行包的文件。

建议通过 Atmosphère **Title Override** 运行
Homebrew，以获得完整可用内存。

------------------------------------------------------------------------

## 编译

需要安装 [devkitPro](https://devkitpro.org/) 以及 Nintendo Switch
开发环境。

``` bash
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-libpng switch-zlib

export DEVKITPRO=/opt/devkitpro
make
```

成功后生成 `phigros_nx.nro`。

FFmpeg 视频后端目前仍在开发中，因此 `switch-ffmpeg`
暂未列为当前稳定版本的强制构建依赖。

------------------------------------------------------------------------

## 存档迁移

Phigros Android 与本项目使用的 PlayerPrefs 存储格式存在差异，因此
Android 存档不能简单直接复制到 Switch。

Android → Nintendo Switch
存档迁移已经完成验证，包括歌曲解锁、成绩、曲绘等本地进度。

Phigros 国服 4.0.0 的 Android → Switch 存档迁移已经完成实机验证。
迁移时需要正确处理 Unity PlayerPrefs 的 URI 编码以及 Switch 平台相关字段。

建议迁移或替换 `prefs.kv` 前先备份原有存档。

------------------------------------------------------------------------

## Roadmap

-   [x] Phigros 国服 4.0.0
-   [x] IL2CPP / GC 运行时适配
-   [x] 60 Hz 时序调整
-   [x] 基础音频、触摸和输入支持
-   [x] Android → Switch 存档迁移验证
-   [ ] FFmpeg 视频解码后端
-   [ ] Unity VideoPlayer / 剧情 CG 支持
-   [ ] 后续 Phigros 国服版本适配

------------------------------------------------------------------------

## 项目来源与致谢

本项目基于
[ChanseyIsTheBest/phigros_nx](https://github.com/ChanseyIsTheBest/phigros_nx)
继续开发。

`phigros_nx_cn` 是独立维护的后续项目，并非原项目的官方更新版本。

底层 Loader / Shim 基础设施来自开源 Nintendo Switch / Android `.so`
Loader 项目体系。感谢原项目以及相关上游项目和贡献者。

------------------------------------------------------------------------

## 免责声明

本项目是非官方社区 Homebrew 项目，与 Pigeon Games /
鸽游无隶属、授权或合作关系。

Phigros、其游戏资源、音乐、美术、程序及相关商标的权利归各自权利人所有。

本仓库仅提供兼容层 / Loader
相关开源代码，不提供游戏本体或游戏资源。请自行确保对所使用的游戏文件拥有合法的获取和使用权限。

------------------------------------------------------------------------

## License

本项目采用 MIT License，详见 [LICENSE](LICENSE)。

------------------------------------------------------------------------

# English

## About

`phigros_nx_cn` is an independently maintained continuation of
[ChanseyIsTheBest/phigros_nx](https://github.com/ChanseyIsTheBest/phigros_nx),
primarily focused on compatibility with the **Chinese Android release of
Phigros** on Nintendo Switch.

**Current target: Phigros CN 4.0.0**

The project is a native compatibility layer / loader for the original
ARM64 Android Unity/IL2CPP build.

This repository does not distribute the Phigros APK, game assets, or
other files from the official game distribution. Users must provide the
required files from their own legally obtained copy.

## Current status

Implemented or adapted:

-   Phigros CN 4.0.0 compatibility
-   CN-specific IL2CPP / GC runtime adaptation
-   Runtime stability fixes
-   60 Hz timing adjustments
-   BGM / OpenSL audio output adaptation
-   GC stop-the-world stability fixes
-   Performance optimizations for high-density charts
-   Android / JNI compatibility improvements
-   Touch and input support
-   Android → Nintendo Switch save migration validation

The current build has passed real-device stability testing and most normal gameplay is functional.

Chart/audio latency can vary with the console, performance or overclock settings, and audio output configuration. Use the in-game latency setting to calibrate for your setup. The port does not force a fixed latency compensation value.

### Known issue: video playback

Unity VideoPlayer / story CG playback is not yet supported.

Some story sequences require video playback and may currently result in
a black screen or prevent progression.

A native Nintendo Switch video backend based on **FFmpeg** is planned to
replace the unavailable Android MediaCodec functionality.

## Building

Requires devkitPro and the Nintendo Switch development environment:

``` bash
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-libpng switch-zlib

export DEVKITPRO=/opt/devkitpro
make
```

## Save migration

Android → Nintendo Switch save migration has been successfully tested
for local progression data, including song unlocks, scores and
illustrations.

Android → Nintendo Switch save migration for Phigros CN 4.0.0 has been validated on real hardware. Migration must correctly handle Unity PlayerPrefs URI encoding and Switch-specific platform fields.

Back up the existing `prefs.kv` before migrating or replacing a save.

## Roadmap

- [x] Phigros CN 4.0.0
- [x] IL2CPP / GC runtime adaptation
- [x] 60 Hz timing adjustments
- [x] Basic audio, touch and input support
- [x] Android → Nintendo Switch save migration validation
- [ ] FFmpeg video decoding backend
- [ ] Unity VideoPlayer / story CG support
- [ ] Support future Phigros CN versions

## Credits

Based on
[ChanseyIsTheBest/phigros_nx](https://github.com/ChanseyIsTheBest/phigros_nx).

This repository is an independently maintained continuation and is not
an official update of the upstream project.

The loader and compatibility infrastructure also derives from the
broader open-source Switch / Android `.so` loader ecosystem and its
contributors.

## Disclaimer

This is an unofficial community Homebrew project and is not affiliated
with or endorsed by Pigeon Games.

No Phigros APKs or game assets are distributed by this repository.

## License

MIT License. See [LICENSE](LICENSE).
