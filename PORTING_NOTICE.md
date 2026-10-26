# Hps3 HarmonyOS 移植声明（v0.1）

## 项目身份

Hps3 是一个面向 HarmonyOS 的社区移植与研究项目，基于公开的 ARMSX3/RPCS3 模拟器代码开展适配。它不是 Sony、PlayStation、Huawei 或 ARMSX3 上游团队发布、认可或维护的官方产品。项目名称仅用于标识本移植工作；相关商标及产品名称归其各自权利人所有。

## 上游来源与修改

- 上游项目：ARMSX3，仓库 `https://github.com/ARMSX2/ARMSX3`
- 本版本基线：提交 `92b931b9fb5eef7f1317cb482917dfc19aceebd9`
- 主要移植内容：HarmonyOS arm64 构建适配、N-API/ArkTS 应用桥接、OHOS Vulkan Surface 与 swapchain 接入、虚拟手柄输入和移动端应用界面。
- 本仓库 `porting/armsx3-harmonyos-v0.1.patch` 记录相对于上述基线的 ARMSX3 源码改动；`hps3-app/` 保存本项目应用层源码。
- v0.1 HAP 是基于该代码构建的实验性未签名调试包，不代表完整、稳定或经所有游戏验收的模拟器版本。

## 许可与版权

ARMSX3/RPCS3 上游代码按其随附的 GNU General Public License version 2（GPL-2.0）发布。上游版权、许可和免责声明继续适用于相应上游代码及其衍生部分。本项目随仓库提供上游 `LICENSE` 文本和移植补丁，分发二进制时应同时提供对应源代码及许可材料。第三方依赖继续受各自目录中的许可证约束。

项目新增或修改的代码按 GPL-2.0 兼容方式提供；除明确标示的上游文件外，不转移第三方代码的权利。所有代码按“现状”提供，不作任何明示或默示担保。

## 固件与游戏内容

本项目及其 GitHub 仓库、Release 不包含或托管 PS3 系统软件固件、游戏 ROM/ISO、游戏密钥、固件解密材料或用户存档；也不提供这些内容的下载链接、网盘地址或获取指引。Hps3 只提供应用与模拟器移植代码。用户如需导入外部内容，应自行确认其来源和使用权，并通过设备上的文件选择器导入。

## 签名与个人信息

公开的 HAP 是未签名调试包。发布内容不包含本机开发者私钥、签名 Profile、设备 ID、构建机绝对路径或测试设备日志。安装者需要自行使用自己的开发者证书和设备 Profile 签名。

## 独立性声明

PlayStation、PS3 及相关产品名称和标识属于其权利人。Hps3 不代表 Sony Interactive Entertainment，也没有与其建立任何官方合作或授权关系。HarmonyOS、Huawei 及相关名称和标识属于其权利人；提及这些名称仅用于说明运行平台。
