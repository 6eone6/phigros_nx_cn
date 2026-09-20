# phigros_nx_cn

> 在 Nintendo Switch 上运行 Phigros 国服 Android ARM64 版本的 Homebrew
> 兼容层 / Loader。

**当前适配版本：Phigros 国服 3.19.5**

`phigros_nx_cn` 是基于
[ChanseyIsTheBest/phigros_nx](https://github.com/ChanseyIsTheBest/phigros_nx)
继续开发的独立维护版本，主要面向 Phigros 国服，并针对 Nintendo Switch
进行了兼容性、稳定性和运行时适配。

本项目不提供 Phigros 本体或游戏资源。运行时需要用户自行从合法获得的
Android 版本中准备所需文件。

[English](#english)

------------------------------------------------------------------------

## 项目状态

目前主要适配：

-   Phigros 国服 3.19.5
-   Nintendo Switch / Atmosphère Homebrew 环境
-   Unity 2022.3 / IL2CPP ARM64 Android 版本

已经完成的主要工作：

-   [x] Phigros 国服 3.19.5 基础适配
-   [x] 国服 IL2CPP / GC 运行时适配
-   [x] Nintendo Switch 运行稳定性修复
-   [x] 60 Hz 时序调整
-   [x] 音频与游戏时序相关修复
-   [x] Android / JNI 兼容层调整
-   [x] 触摸与输入适配
-   [x] Android → Switch 存档迁移验证

当前版本已经可以正常进入并游玩大部分游戏内容。

### 已知问题：剧情 CG / VideoPlayer

剧情 CG / Unity VideoPlayer 暂不可用。

部分主线内容会强制播放剧情视频。当前 Android MediaCodec / Unity
VideoPlayer
视频解码后端尚未完成，因此进入这些视频时可能出现黑屏或无法继续的问题。

计划实现基于 **FFmpeg** 的 Nintendo Switch 原生视频播放后端，以替代
Android MediaCodec 相关功能。

------------------------------------------------------------------------

## 安装与运行

你需要从自己合法获得的 **Phigros 国服 3.19.5 Android
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

全解锁存档目前还没有整理为3.19.5使用的版本，后续计划将在适配3.20.0版本时加入Release。

------------------------------------------------------------------------

## Roadmap

-   [x] Phigros 国服 3.19.5
-   [x] IL2CPP / GC 运行时适配
-   [x] 60 Hz 时序调整
-   [x] 基础音频、触摸和输入支持
-   [x] Android → Switch 存档迁移验证
-   [ ] FFmpeg 视频解码后端
-   [ ] Unity VideoPlayer / 剧情 CG 支持
-   [ ] 整理并发布全解锁存档
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

**Current target: Phigros CN 3.19.5**

The project is a native compatibility layer / loader for the original
ARM64 Android Unity/IL2CPP build.

This repository does not distribute the Phigros APK, game assets, or
other files from the official game distribution. Users must provide the
required files from their own legally obtained copy.

## Current status

Implemented or adapted:

-   Phigros CN 3.19.5 compatibility
-   CN-specific IL2CPP / GC runtime adaptation
-   Runtime stability fixes
-   60 Hz timing adjustments
-   Audio and timing fixes
-   Android / JNI compatibility improvements
-   Touch and input support
-   Android → Nintendo Switch save migration validation

Most normal gameplay is currently functional.

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

A ready-to-use fully unlocked save for Phigros CN 3.19.5 has not yet
been prepared for release. It is currently planned to be included in
a future Release alongside the adaptation for Phigros CN 3.20.0.

## Roadmap

- [x] Phigros CN 3.19.5
- [x] IL2CPP / GC runtime adaptation
- [x] 60 Hz timing adjustments
- [x] Basic audio, touch and input support
- [x] Android → Nintendo Switch save migration validation
- [ ] FFmpeg video decoding backend
- [ ] Unity VideoPlayer / story CG support
- [ ] Prepare and publish a fully unlocked save
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
