# 可选 GameZone 窄修复 · 0.1

某些内核的 lenovo_wmi_gamezone 在此旧 BIOS 上调用不存在的 SmartFan 方法，产生 probe 错误。
本修复仅对 LENOVO / 80Y4 / Y720-15IKB / BIOS 4GCN38WW 提前返回 -ENODEV，
跳过现代 platform_profile 探测，让该 GUID 可供旧机型风扇驱动绑定；不整体禁用 WMI。

compat/gamezone/ 是先前审查的 CachyOS 7.1.6 接口参考源码及 headers，保留原 Linux 作者归属和 GPL。
附带 skip-legacy-smartfan.patch 展示精确改动，适合在目标内核对应源码中审查后移植。
完整参考模块的项目版本为 0.1；不代表任意 Linux 版本 ABI 兼容。
若系统已经包含等价修复或原模块 probe 失败但旧驱动正常绑定，通常不需要替换模块。

**install.sh 不安装该可选模块，不覆盖 vendor 文件，不屏蔽 WMI。**
手动编译需使用匹配该源码的内核 headers/工具链：

```sh
make -C compat/gamezone KDIR=/lib/modules/$(uname -r)/build LLVM=1
```

构建成功仍需独立现场验证，尤其是较新内核的 WMI/platform_profile API。
不要将该参考 .ko 跨内核安装。若临时人工加载，先保证风扇受控、旧模块已安全卸载，
结束后卸载临时模块并 modprobe 系统自带 lenovo_wmi_gamezone；原固件报错可能再次出现。
该模块不纳入 DKMS，避免后续内核升级时自动替换系统驱动。
