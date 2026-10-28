# 驱动接口 · 0.1

仅适用 LENOVO / 80Y4 / Y720-15IKB / BIOS 4GCN38WW。两个 CLI 仍会核验设备身份。

## 风扇 hwmon

枚举 /sys/class/hwmon/hwmon*，name=y720_cooling，父设备 driver=y720_cooling。
加载默认 allow_control=0 只读；显式重新加载 allow_control=1 才开放控制。

| 文件 | 含义 |
|---|---|
| fan1_input / fan2_input | 实际 RPM，分辨率 100 RPM，两路传感器 |
| fan1_target | 一个共享目标，1500–5100，向上量化 100 RPM |
| pwm1 | 一个联动归一化请求，0–255，非实测占空比；0 不停转 |
| pwm1_enable | 2 auto，0 boost，1 curve |

没有 pwm2、fan2_target、pwm1_mode 或频率属性。默认模式下目标仅暂存，进入 curve 才应用。
相同目标不重复发送；不同目标有 15 秒节流，100 RPM 变化另需稳定 10 秒。
EC 新目标会重新初始化闭环，可能暂时降低实际转速；不要使用秒级激进曲线。
目标是最新请求，不等于已发送/收敛；多个 sysfs 文件不构成原子事务。

父 WMI 设备诊断：requested_mode、safety_fault、last_error、target_writes、mode_writes、ramp_low_samples。
requested_mode=-1 表示只读加载尚无原生请求。safety_fault 是告警标志，不代表已自动升速。
CPU/GPU 温度、低速和通信检查仅诊断；curve 中保留并重试最新请求，不切 auto/boost/固定 5100。
独显休眠不强制唤醒；应用需承担完整 CPU/GPU 曲线与温度异常策略。
卸载/关机/休眠交回 auto，恢复重发原曲线；EC 失联和内核崩溃不能保证恢复。

## 输入模块

y720_hid 只处理 USB 048d:c100 v1.10、受支持 DMI 及精确 163 字节描述符。
不发送 EC、ACPI 或任何背光 Feature。已知映射如下：

| 键/原始事件 | Linux 事件 |
|---|---|
| Fn+F4 consumer 0x54 | KEY_MICMUTE |
| Fn+F6 keyboard F23 | KEY_TOUCHPAD_TOGGLE |
| Fn+F8 consumer 0x57 | KEY_CAMERA_ACCESS_TOGGLE |
| Fn+空格 consumer 0x50 | KEY_KBDILLUMTOGGLE，仅上报 |
| Fn+小键盘7 consumer 0x53 | KEY_PROG1 |
| 不带修饰键 F22 录屏键 | KEY_RECORD（map_record=0 可禁用） |
| 带修饰键 F22 | 保留 F22 |
| Fn+F7 的精确 `03 00` 报告 | KEY_RFKILL 按下/松开，仅 `radio_zero_pulse=1` 时 |

普通键、音量、屏幕亮度和标准媒体 HID 保持通用映射；动作需桌面支持。
默认不将零值报告合成飞行模式。兼容开关仅加载时可设，限制与风险见 [FNF7.md](FNF7.md)。
驱动只上报标准输入事件，不直接写 rfkill；实际无线状态应查询 rfkill，不能从按键计数推断。
输入设备父 HID 的只读属性 `radio_zero_reports` 为精确零报告计数，
`radio_zero_pulses` 为合成脉冲数，`radio_zero_suppressed` 为启用时被过滤的零报告数。
初始化期间也可计数；计数随重新探测归零。摄像头物理断开不能据事件判断工作正常。
相同 ID 的未核验设备保留普通 HID 解析；verified_mapping 属性区分是否启用专用映射。

## 用户态背光

独立 I2C BUS=0x18、VID=048d、PID=837a，名称前缀 ITE33D1:，driver 必须为 hid-generic。
不要绑定 USB 输入设备或旧 RGB 内核模块。精确 46 字节描述符 SHA256：
`587474323e725222dfa27cb66742b145b0f244e089cf2649f7d6bc0f86f50281`。
打开后检查字符设备号、HIDIOCGRAWINFO、HIDIOCGRDESCSIZE/HIDIOCGRDESC 和 driver；不符即拒绝。
产品 ID 以 uint16_t 位型比较，不能被有符号 short 误判。

```
cc 00 EFFECT COLOR BRIGHTNESS ZONE 00  # zone 0..3 各一次
cc 09 00 00 00 00 00                # 最后一条提交
```

使用 HIDIOCSFEATURE(7)，返回必须等于 7。完整 profile 串行提交，失败立即停止，不自动补交。
效果/调色板/亮度见 CLI.md。未有可靠配置读回接口，status 只能确认身份，颜色仍未知。
传输成功不等于视觉验证，配置不能被磁盘缓存冒充读回。

## 并发和权限

hwmon 写入由内核共享锁管理，CLI 另用非阻塞协作 flock；背光同时锁全局文件及 hidraw fd。
锁文件 O_NOFOLLOW、所有者/类型/nlink/权限核验；不删除锁文件。其他不配合的软件仍可能竞争。
未来 GUI/权限代理只开放固定程序及有限操作，不开放任意端口、寄存器或任意 root 文件读写。
