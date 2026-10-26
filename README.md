# Hps3 v0.1

HarmonyOS 原生 PS3 模拟器移植实验，基于 ARMSX3/RPCS3。当前版本用于公开移植进度和验证构建，仍属实验版，不能视为稳定可玩版本。

## v0.1 状态

- 能在 HarmonyOS 设备上加载模拟器核心并创建 Vulkan Surface。
- 游戏输入界面支持虚拟按键与双摇杆；游戏覆盖层提供可拖动 FPS 悬浮球和虚拟按键显隐。
- 已有设备运行记录覆盖启动到游戏画面；稳定战斗、暂停/继续、即时存档和完整退出恢复流程尚未全部验收。
- 本版本 HAP 是未签名的 arm64 调试构建，安装前需要使用自己的 HarmonyOS 开发者签名配置重新签名。

## 下载与内容范围

HAP 位于 GitHub Releases。仓库和发布包**不包含、不托管、不提供下载链接或网盘分发**任何 PS3 系统固件、游戏 ROM/ISO、密钥或用户存档。用户需要自行准备有权使用的内容，并通过应用内文件选择器导入。

## 移植与许可

本项目是社区 HarmonyOS 移植工作，与 Sony、PlayStation、Huawei 或 ARMSX3 上游团队没有官方关联或背书。移植来源、修改范围、许可及内容边界见 [移植声明](PORTING_NOTICE.md)。

## 源码

- `hps3-app/`：HarmonyOS ArkTS 应用与 N-API 桥接源码。
- `porting/armsx3-harmonyos-v0.1.patch`：针对声明的 ARMSX3 基线提交的移植补丁。
- 上游基线和应用方法见移植声明。HarmonyOS 构建需要 DevEco Studio、HarmonyOS SDK/NDK 和相应本地构建依赖；项目不发布签名私钥、开发者 Profile 或设备授权信息。

## 发布文件

从 [GitHub Releases](https://github.com/ljq123ok/Hps3/releases) 获取 `hps3-v0.1.0-ohos-arm64-unsigned-sanitized.hap`。该文件未签名，不能直接安装；请使用自己的证书和设备 Profile 签名。不要使用他人的私钥、Profile 或设备授权。
