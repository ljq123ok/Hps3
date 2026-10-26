# Hps3 v0.1

本次发布提供 HarmonyOS arm64 未签名调试 HAP 与对应的应用源码、ARMSX3 HarmonyOS 移植补丁。

## HAP

- 文件：`hps3-v0.1.0-ohos-arm64-unsigned-sanitized.hap`
- 类型：未签名的 arm64 调试构建；安装前需使用自己的开发者证书和设备 Profile 签名。
- SHA-256：`188bf0a5293d946828c73f5b2596b1df082b9e2860ccd30df2256d6980ede0bd`
- 清理：剔除 source map 与包上下文元数据，清除构建机绝对路径；不含开发者私钥、签名 Profile 或设备 ID。

## v0.1 内容

- HarmonyOS 原生 ArkTS 应用与 N-API 桥接。
- ARMSX3 Vulkan Surface/swapchain 移植补丁。
- 全屏虚拟控制布局和可拖动 FPS 悬浮球。
- 实验状态：本版本不保证稳定游戏运行；暂停/继续、即时存档和完整生命周期恢复尚未全部验收。

## 内容边界

此仓库与 Release 不包含、不托管、不提供下载链接或网盘地址来分发 PS3 系统固件、游戏 ROM/ISO、密钥或用户存档。请阅读仓库中的 `PORTING_NOTICE.md`。
