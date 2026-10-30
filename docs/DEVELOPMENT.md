# 开发与驱动接口

版本统一读取根目录 `VERSION`，当前为 0.1.1。应用 ID 为 `org.y720.Control`，
配置目录为 `$XDG_CONFIG_HOME/y720-control`，默认 `~/.config/y720-control`。
JSON/profile 的 `schema_version=1` 与软件、BIOS 和内核版本互不替代。

## 构建、测试与发布

```sh
make
make gui
make test
make modules KDIR=/lib/modules/$(uname -r)/build LLVM=1
./scripts/build-debs.sh
Y720_TEST_DEB_DIR="$PWD/dist" python3 tests/test_debs.py -v
```

LLVM=1 仅适用于对应的 Clang 内核。C CLI 依赖 libc/libm，启用严格告警、FORTIFY、
PIE、栈保护和 RELRO/NOW。GUI 需要 GTK 4.16+、libadwaita 1.7+、JSON-GLib、GIO、
GdkPixbuf；X11 快捷键另用 libX11，`meson setup … -Dx11=disabled` 可禁用该后端。
Wayland 快捷键使用 GIO D-Bus，不增加 libportal 构建依赖。运行时需要支持
GlobalShortcuts 的 xdg-desktop-portal 桌面后端。构建使用 Meson、Ninja、pkg-config 和 Python 3。
测试另需 Clang ASan/UBSan、gtk4-broadwayd；缺少依赖的项明确跳过，不算通过。
GTK 测试关闭全局缓存的泄漏检测，仍检查越界与未定义行为，不代表已证明没有任何泄漏。

`fan/tests/thermal/` 是独立、显式加载的内核集成测试：复用生产 thermal 回调，
使用模拟温度及无硬件写入的传输替身，不访问 PCI、WMI、EC 或真实风扇，也不进入发行包。
生产驱动没有模拟温度模块参数或用户态温度写接口。模拟回归不替代真实散热验收。

```sh
# 可选：仅构建集成测试模块，不加载
make -C /lib/modules/$(uname -r)/build M="$PWD/fan/tests/thermal" LLVM=1 modules
```

安装器验证全部目标后才写入，按清单卸载并保留个人配置，拒绝修改过的或外来文件。
DESTDIR 用于隔离测试；GUI 构建和安装前缀须一致。DKMS 失败向上传递，已有注册不覆盖。
deb 构建只更新本项目输出，不清空 dist；运行依赖由 dpkg-shlibdeps 生成。
源码升级前先用旧版卸载器卸载，deb 则交给包管理器，不混用。

## hwmon 与 thermal

在 `/sys/class/hwmon/hwmon*` 中查找 `name=y720_cooling`，并核验父驱动。
`allow_control=1` 且精确机型/BIOS 核验通过后才开放 EC 控制和温度属性。
通信共用同一锁，只允许已验证的固定端口、寄存器和命令，不提供任意读写接口。

| hwmon 属性 | 含义 |
|---|---|
| `fan1_input` / `fan2_input` | 两路实际 RPM，分辨率 100 RPM |
| `fan1_target` | 一个联动目标，1500–5100，向上量化到 100 RPM |
| `pwm1` | 同一目标的归一化请求，0–255；0 不停转 |
| `pwm1_enable` | 0 原厂 boost；1 外部/手动调速；2 原厂 auto；3 内核 thermal 曲线 |
| `temp1_input` / `temp1_label` | 单一控制温度，只读，毫摄氏度；实际来自 CPU 封装 |

不提供 pwm2、fan2_target、电气模式或频率属性。auto/boost 下的目标只暂存，模式 1 才应用；
模式 3 下直接写目标会释放内核曲线，交给外部控制者。相同目标不重置 EC 闭环，
不同目标最多每 15 秒提交，100 RPM 小变更还需稳定 10 秒。目标改变可能短暂降低实际 RPM，
请求被接受不代表已应用或收敛。

父 WMI 设备属性：

| 属性 | 含义 |
|---|---|
| `control_api` / `curve_backend` | 2 / linux-thermal 表示支持本版曲线；不可用时 API 为 1 |
| `curve_profile` | 六个整数 RPM，对应 40/50/60/70/80/90°C；完整校验后原子接管 |
| `curve_active` / `curve_error` | thermal 曲线是否接管，以及最近温度错误 |
| `curve_temperature` | 有效曲线温度；不可用时 ENODATA，不返回旧缓存 |
| `temperature_source` | x86_pkg_temp；单一控制温度源，不依赖独显状态 |
| `requested_mode` | EC 请求模式，内部软件模式仍为 1，与 hwmon 的 3 区分 |
| `safety_fault` / `last_error` | 温度、测速或通信诊断，不代表已自动提速 |
| `target_writes` / `mode_writes` / `ramp_low_samples` | 写入与过渡计数，不是模式状态 |

六个 RPM 必须是非递减的 1500–5100 整百数。驱动注册 `y720-curve` thermal zone 与
`y720-linked-fans` cooling device，使用 `step_wise`：以 1°C 分辨率在节点间插值，
向上量化到 100 RPM，降温保留 2°C 滞回。thermal 负责温度采样和选档，guard 只校验、
限频并重试最新 RPM。更高的 cooling state 不得对应更低 RPM。

加载时 thermal zone 为 disabled，完整曲线提交或显式选择模式 3 后才启用。
CLI/GUI 退出不停止；`curve stop` 释放内核选档、保留当前软件目标，不切 auto/boost。
手动模式选择和外部 hwmon 写入会停止内核接管。不要同时改写 thermal policy、cur_state
和 hwmon；不使用同一协作锁的写入者仍会竞争，多文件查询也不是原子快照。

锁顺序为 thermal 管理锁 → thermal core 锁 → EC 数据锁。持 EC 锁时不进入 thermal core，
迟到回调先检查控制权，不覆盖新控制者。暂停/卸载先阻止新写入，再停止采样和后台工作。
休眠交回固件 auto，恢复后重建原策略。硬件失联、内核崩溃不保证恢复。

控制温度不可读时向 thermal 返回 EAGAIN，保留重试；curve_error 保留真实 errno，
不伪造温度，也不自动退回默认模式。首次接管须有有效温度。

本版只有一个温度源：内核 thermal 的 x86_pkg_temp（CPU 封装）。驱动直接调用
thermal_zone_get_temp，温度采样、曲线选档和 RPM 提交都在内核内完成。
CLI 和 GUI 只显示一个 temperature 对象，不区分 CPU/GPU 控制分支，也不调用 NVIDIA。
不提供外部温度注入接口、遥测有效期、采样服务或与用户进程存活相关的控制心跳。
该读数不代表整机最高温度；独显单独高负载的散热表现仍需验证。

## CLI 与 GUI 契约

两个 C CLI 的正式路径固定在 /sys、/dev、/run，没有环境变量 sysroot 绕过身份校验。
各自使用 `/run/y720-fan.lock`、`/run/y720-backlight.lock` 的非阻塞协作 flock，
背光另锁 hidraw fd。锁核验所有者、类型、nlink、权限和 O_NOFOLLOW，不删除锁文件。
命令、配置和 JSON 字段见 [CLI.md](CLI.md)。

GUI 使用 GIO 异步子进程、argv 数组和 stdin，不拼接 shell。核验程序名、schema、版本、
退出码和状态结构；旧查询不能覆盖新写入回执。同版号不足以证明曲线能力，需检查
kernel_curve.supported。只有成功回执才确认提交，旧回执不能覆盖新编辑的节点。
节点拖动停止 350ms 后才提交最新整条曲线，避免每个鼠标事件都调用 CLI 或访问 EC。
外部控制者接管后不自动抢回；GUI 只展示温度，不根据缓存或缺失读数计算 RPM。

CLI 与 GUI 同目录优先，再查 PATH。开发可用 Y720_FAN_BIN、Y720_BACKLIGHT_BIN，
但覆盖路径不自动提权。权限不足时最多重试一次 sudo -n，只允许 root 所有且父目录
不可被普通用户改写的受信 CLI。sudoers 只授予 sudo/wheel 固定状态、模式、整百 RPM、
curve apply/stop 和背光 stdin 命令，不放行任意文件路径、shell 或宽泛 polkit。
输入 uaccess 精确匹配专用设备，安装只定向刷新相应节点，不给整个 input 组权限或 0666。

普通组合键按会话选择 X11 XGrabKey 或 Wayland GlobalShortcuts Portal。
Portal 使用独立的普通用户会话总线连接，先向 Registry 注册 `org.y720.Control`；
对应 desktop 文件必须已安装。动作 ID 固定为 light-next、fan-toggle，GTK 组合键转换为 XDG 格式，
例如 `<Control><Shift>0` → `CTRL+SHIFT+0`。桌面可以修改首选绑定，设置页展示实际 trigger_description。
CreateSession/BindShortcuts 全程异步，先订阅 Response 再发请求，区分等待、拒绝、部分授权和错误。
只接收当前 Portal 进程、当前会话且已授权的动作，Activated/Deactivated 去重；
Wayland 全局模式不再为同一动作注册窗口内组合键，避免双重执行。
录制、重新配置和退出均关闭请求与会话并断开专用连接，延迟回调不能访问已销毁的应用。
服务断开后提示重新启用，不反复弹出授权；专用 evdev 功能键独立工作，不扩大输入权限。
`test-portal-hotkeys` 在私有 D-Bus 上测试真实客户端，不模拟硬件写入，也不向真实桌面注册按键。

GTK 回调仅在主上下文更新 UI，所有者销毁后不执行 UI 回调。取消先 SIGTERM，1.2 秒后
仍未退出再终止并回收。托盘处理 Event/EventGroup；退出禁止新动作、取消子进程，
最多等待 3 秒回收，不为退出写硬件恢复命令。翻译覆盖标签、格式、动态提示和对话框，
保留用户自定义方案名；新增文本须补英文目录和回归测试。

## 键盘与背光

输入模块限定 USB 048d:c100 v1.10、精确 DMI/BIOS 和 163 字节完整描述符。
不匹配的设备保留普通 HID 解析。专用映射：Consumer 0x54 → MICMUTE，Keyboard F23 →
TOUCHPAD_TOGGLE，Consumer 0x57 → CAMERA_ACCESS_TOGGLE，0x50 → KBDILLUMTOGGLE，
0x53 → PROG1。NumLock 数字层、标准媒体和音量/亮度沿用通用路径，不猜测桌面 NumLock 状态。

Fn+F7 的零报告兼容默认关闭，首次测试需有线 SSH 或外接键盘：

```sh
sudo scripts/keyboard-bind.sh restore
sudo modprobe -r y720_hid
sudo modprobe y720_hid radio_zero_pulse=1
sudo scripts/keyboard-bind.sh bind
scripts/keyboard-bind.sh status
rfkill
```

已加载参数须安全重载，重复 modprobe 不修改参数。精确 `03 00` 合成为 KEY_RFKILL，
无线动作由系统决定；探测后 2 秒、恢复后 5 秒过滤，脉冲间隔至少 150ms。同报告出现
非零值后停止零值解释，证据跨休眠保留。这不能绝对区分按键、初始化或释放，过滤也可能
忽略真实按键，不保证所有固件时序无误触。verified_mapping 表示身份匹配，
radio_zero_reports/pulses/suppressed 只是计数。误触时执行 keyboard-bind.sh restore；
卸载不会自动恢复用户选择的无线状态。

背光另属 BUS=0x18、VID=048d、PID=837a、ITE33D1: 前缀的 hid-generic 设备，
46 字节描述符 SHA256 为 `587474323e725222dfa27cb66742b145b0f244e089cf2649f7d6bc0f86f50281`。
打开前后都核验身份。完整配置发送四条 `cc 00 EFFECT COLOR BRIGHTNESS ZONE 00`，
再发送 `cc 09 00 00 00 00 00`；每次 HIDIOCSFEATURE(7) 必须返回 7。
部分失败或取消立即停止，不自动补交。status 不打开 hidraw，保存值、提交值均不冒充读回值。

## GameZone 与许可

compat/gamezone/skip-legacy-smartfan.patch 只让此机型/BIOS 跳过不存在的 SmartFan
探测，不整体禁用 WMI。参考源码基于特定内核，须对照目标内核审查构建，不跨内核复制 .ko
或加入通用 DKMS；安装器和 deb 不安装它。已有等价修复或风扇可正常绑定时通常无需替换。

保留逐文件 SPDX、上游作者和根目录 LICENSE。GPL 内核、MIT 用户态分开处理，
不能把整仓库宣称为 MIT。编译、模拟回归及旧版记录不能
冒充本版完整实机验收。
