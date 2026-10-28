# Y720 Nerve Linux

联想 Y720-15IKB 的 Linux 风扇、键盘功能键与四区背光控制。

支持：**LENOVO 80Y4 / Y720-15IKB / BIOS 4GCN38WW / Linux x86_64**。程序会核验设备；
其他型号、BIOS 或内核不保证兼容。

## 组件

| 组件 | 说明 |
|---|---|
| `y720_cooling` | 风扇内核驱动，标准 hwmon 接口 |
| `y720_hid` | 键盘功能键内核驱动 |
| `y720-fan` / `y720-backlight` | CLI 控制程序 |
| `y720-control` | GTK4 + libadwaita GUI 控制程序 |

## 安装

deb 包（推荐）：

```sh
./scripts/build-debs.sh
sudo apt install ./dist/*.deb
```

| 包 | 内容 |
|---|---|
| `y720-cooling-dkms` | 风扇 DKMS 驱动 |
| `y720-hid-dkms` | 键盘功能键 DKMS 驱动 |
| `y720-control` | CLI + GUI 程序 |

DKMS 安装需要 `dkms`、`make` 和匹配的内核 headers；安装器不会自动加载模块或改启动项。

源码安装：

```sh
sudo apt install build-essential libgtk-4-dev libadwaita-1-dev libjson-glib-dev \
  libgdk-pixbuf-2.0-dev libx11-dev dkms
make
make gui
sudo ./install.sh              # CLI + 驱动 + GUI
sudo ./install.sh --cli        # 仅 CLI
sudo ./install.sh --gui        # 仅 GUI
sudo ./install.sh --drivers    # 仅驱动
```

## 驱动与按键映射

```sh
sudo modprobe y720_cooling                 # 只读测速
sudo modprobe -r y720_cooling
sudo modprobe y720_cooling allow_control=1 # 允许调速

sudo modprobe y720_hid
sudo scripts/keyboard-bind.sh bind
scripts/keyboard-bind.sh status
```

键盘驱动保留普通键、音量、亮度和标准媒体键，额外映射：

| 按键 | Linux 事件 |
|---|---|
| Fn+F4 | `KEY_MICMUTE` |
| Fn+F6 | `KEY_TOUCHPAD_TOGGLE` |
| Fn+F8 | `KEY_CAMERA_ACCESS_TOGGLE` |
| Fn+空格 | `KEY_KBDILLUMTOGGLE`，仅上报，不直接控制背光 |
| Fn+小键盘 7 | `KEY_PROG1` |
| 不带修饰键的 F22 录屏键 | `KEY_RECORD`；`map_record=0` 可禁用 |
| Fn+F7 的精确 `03 00` 报告 | `KEY_RFKILL`；仅 `radio_zero_pulse=1` 时启用 |

Fn+F7 的实际飞行模式动作由系统 rfkill/桌面策略处理；启用方法、过滤窗口与回滚见
[Fn+F7](docs/FNF7.md)。重新绑定键盘前请保留有线 SSH 或外接键盘恢复通道。

## 可选 GameZone 兼容

部分内核的 `lenovo_wmi_gamezone` 会在这台旧 BIOS 上探测不存在的 SmartFan 方法。
`compat/gamezone/` 提供限定 LENOVO / 80Y4 / Y720-15IKB / BIOS 4GCN38WW 的窄修复，
只跳过该探测，不整体禁用 WMI，也不替换厂商文件。

`install.sh` 和 deb 包**不会**安装这个模块。需要时按 [GameZone 说明](docs/GAMEZONE.md)
使用匹配的内核 headers 手动构建、审查并现场验证；不要跨内核安装预编译 `.ko`，
也不要把参考模块加入 DKMS。

## CLI

```sh
y720-fan --json status
sudo y720-fan mode auto
sudo y720-fan mode boost
sudo y720-fan set --rpm 4000

y720-backlight --json status
sudo y720-backlight set --colors red blue green white --levels 0 1 2 3
sudo y720-backlight off
```

## 安全与限制

驱动不提供任意 EC 访问；风扇只有一个联动通道。GUI 曲线由用户态采样温度并通过
`y720-fan set --rpm` 提交，不是驱动内置曲线。背光不回读真实颜色/亮度，界面不会伪称
已读回；写入失败或不确定时保留错误信息，不自动回滚。

源码安装默认 CLI 到 `/usr/local/bin`，deb 安装到 `/usr/bin`；polkit 策略按安装前缀生成。

## 许可

内核驱动 GPL-2.0-only，CLI 与 GUI MIT。详见 [LICENSE](LICENSE) 与 [LICENSES/](LICENSES/)。
GUI 图标素材来自 [stanislav7766/extreme-cooling](https://github.com/stanislav7766/extreme-cooling)。
