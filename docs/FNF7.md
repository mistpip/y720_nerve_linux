# Fn+F7 飞行模式兼容项 · 0.1

## 支持与启用

仅适用 LENOVO 80Y4 / Y720-15IKB / BIOS 4GCN38WW、USB 048d:c100 v1.10，
且必须匹配驱动内的完整 163 字节报告描述符。背光 I2C 接口不参与。
本机在通用驱动下，物理 Fn+F7 只送出 `03 00`，没有标准按键事件。
兼容项将这个精确报告解释为一次 `KEY_RFKILL`（247）按下、松开；不直接调用无线开关接口。
桌面/系统无线策略决定实际动作，因此不能承诺任意桌面均自动切换。

完成 README 的驱动安装后，保留有线 SSH 或外接键盘，手动执行：

```sh
sudo scripts/keyboard-bind.sh restore
sudo modprobe y720_hid radio_zero_pulse=1
sudo scripts/keyboard-bind.sh bind
scripts/keyboard-bind.sh status
sudo cat /sys/module/y720_hid/parameters/radio_zero_pulse
rfkill
```

参数是只读的加载参数，不能通过 sysfs 动态修改。单纯重复 modprobe 不会修改已加载参数。
不创建 modprobe.d、modules-load.d 或任何开机自动启用项。首次启用建议使用有线连接，
避免 Fn+F7 中断无线 SSH；本驱动不代替 rfkill 管理器，也不会擅自恢复用户切换后的无线状态。

## 保护与限制

- 默认 `radio_zero_pulse=0`：所有零值报告仅计数，不合成飞行模式。
- 完成探测后的前 2 秒、恢复后的前 5 秒过滤零值脉冲；此时按键可能被有意忽略。
- 两次脉冲至少间隔 150 毫秒；不使用定时轮询，不产生长按自动重复。
- 一旦收到同一报告 ID 的非零按钮值，本次绑定期停止零值解释，保持正常 HID 非零路径，
  避免把标准按下后的释放重复解释成点击；此证据跨睡眠恢复保留，重新探测后才清除。
- 单凭 `03 00` 无法证明来源一定是按键，而非固件初始化或释放。采集曾有一次混合对照
  与单独 Fn 的独立复测不一致；后续独立 Fn 及启用后的 Fn 三次对照均未触发。
  因此保留显式选择，不声称已排除所有时序/固件变体中的误触发。
- 实际系统睡眠/恢复尚未验证；时间过滤不是固件正确性的保证。

防御性输入校验和生命周期过滤依据嵌入式/内核模块调试规范实现；不发送 EC、ACPI、
HID Feature 或 RGB 命令。既有麦克风、触控板和录屏映射不变，所有软件版本仍为 0.1。

## 诊断与回滚

通过 `scripts/keyboard-bind.sh status` 确认物理设备为 `y720-hid`。
HID 设备目录中的 `verified_mapping` 应为 1，`radio_zero_reports` 计数原始零报告，
`radio_zero_pulses` 计数合成事件，`radio_zero_suppressed` 计数启用时被过滤的零报告。
它们不表示当前飞行模式；GUI 应通过标准 rfkill 状态获取无线状态。

若出现误触发，执行 `sudo scripts/keyboard-bind.sh restore` 恢复通用键盘；
如需保留其余新映射，再使用不带参数的 `sudo modprobe y720_hid` 并执行 bind。
无线状态独立恢复，例如用户确实希望全部打开时才执行 `sudo rfkill unblock all`。
不要把驱动卸载等同于恢复无线状态。专项测试范围见 [COMPATIBILITY.md](COMPATIBILITY.md)。
