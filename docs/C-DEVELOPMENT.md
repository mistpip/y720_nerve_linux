# C 开发与构建 · 0.1

## 目录

fan/kernel 与 keyboard/kernel 为 GPL 内核源码，dkms.conf 声明版本 0.1。
userspace/fan 与 userspace/backlight 为两个 MIT C11 CLI，common/cli.h 仅编译期复用，无共享运行库。
compat/gamezone 为可选特定内核参考/窄修复，不参与默认安装。
tests 与各组件 tests 是成品回归测试，不含实机采集、压力测试或研究脚本。

```sh
make
make test
make -C userspace CC=clang BUILD=build/clang
# 内核构建示例，选择目标环境确切 headers 和编译器：
make modules KDIR=/lib/modules/$(uname -r)/build
# Clang 构建的内核：
make modules LLVM=1 KDIR=/lib/modules/$(uname -r)/build
```

用户态运行只需系统 libc/libm。测试另需 Python 3 与 Clang ASan/UBSan，不是程序运行依赖。
默认开启严格告警、格式检查、FORTIFY、栈保护、PIE、RELRO/NOW；测试不运行实机命令。
内核 API 随版本变化；通过某一组 headers 编译不构成其他内核兼容保证，Secure Boot 需正规签名。

## status 与实现边界

fan status 读取已验证 hwmon 的多文件快照，状态字段/错误码见 CLI.md。
backlight status 与 info 使用同一只读路径，仅查 sysfs 元数据，不打开 hidraw、发 ioctl 或申请写锁。
没有已验证的灯光配置读回；不要增加“上次写入缓存就是当前状态”的逻辑。
生产路径固定 /sys、/dev、/run；没有环境变量或任意 sysroot 参数绕过验证。
测试用独立编译单元替换 syscall，不链接进正式 CLI。

profile 解析为有界、无递归/动态分配的专用 schema 解析器，最大 4096 字节。
有效字段/效果/颜色均 ASCII，支持 ASCII Unicode 转义及可选 UTF-8 BOM，拒绝其他名称/非法输入。
若扩展 Unicode 自定义标签，需另行实现完整 UTF-8/代理对规则，而非放宽现有边界。
写入检查长度；SIGINT/SIGTERM 仅设 sig_atomic_t 标志，正常控制流释放 fd/flock，不在信号处理器写硬件。
无隐式回滚、自动提权、温度策略、心跳、背光配置缓存或内核 RGB 操作。

## 版本与许可

VERSION、CLI_VERSION、MODULE_VERSION、PACKAGE_VERSION 均为 **0.1**。
JSON schema_version=1 和 profile schema_version=1 不是软件版本，不改成 0.1；
BIOS 4GCN38WW、HID v1.10、硬件 ID 与内核版本也不是项目版本，不得替换。
fan CLI 写入仅接受本版驱动；变更版本时同步检查 tests/test_release.py。
不兼容 JSON 字段类型/语义需提高 schema，错误 details 可添加可选字段。
保留各文件 SPDX 和上游版权；内核 GPL 与用户态 MIT 不能混同。见仓库 LICENSE。

## 安装维护

先用普通用户 make，install.sh 才复制 CLI；DKMS 构建是显式驱动安装步骤，需要 root。
默认安装两驱动和两 CLI，不自动加载或设置启动。可单独 --cli/--drivers；安装依赖由用户另行完成。
CLI 安装前检查全部目的文件，拒绝外部修改/符号链接；卸载保留目录和所有源代码。
DKMS 拒绝覆盖当前 0.1 源码/注册；失败状态保留，审查后手动 dkms remove，不做危险自动清理。
升级使用旧版卸载器先移除旧的匹配文件；保留每次安装的构建产物和文档供逐字节卸载校验。
