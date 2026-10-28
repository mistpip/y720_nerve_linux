# CLI 0.1

两个独立 C11 程序：`y720-fan` 与 `y720-backlight`。不提供合并总控或守护进程。
全局选项必须在命令前：`--json`、`--device NAME`、`--dry-run`。
NAME 是 hwmonN 或 hidrawN，不接受任意路径；手选也核验精确 DMI、驱动与接口身份。
`--help`/`--version` 无硬件访问。成功写入静默；状态默认 key=JSON值；错误只到 stderr。

## 风扇

```sh
y720-fan --json status
sudo y720-fan mode auto
sudo y720-fan mode boost
sudo y720-fan set --rpm 4001
sudo y720-fan --json set --rpm 4000 --wait 40 --tolerance 250
sudo y720-fan set --pwm 128
y720-fan --json --dry-run set --pwm 0
```

status 为只读 hwmon 快照，返回 `device,driver_version,controls_available,mode,mode_raw,target_rpm,pwm,fan_rpm,diagnostics`。
mode 为 auto/boost/curve，原始值 2/0/1。只读加载时控制字段为 null，而非 0/boost；两路 RPM 始终来自测速。
diagnostics 包含 requested_mode、safety_fault、last_error、target_writes、mode_writes、ramp_low_samples；缺失项 null。
多属性读取不是原子快照；短暂不一致时重读，不推断设置成功。

set 必须且仅选 `--rpm 1500..5100` 或 `--pwm 0..255`。RPM 向上量化为 100 的整数倍。
PWM 是归一化目标，不是电气占空比，0 仍对应 1500 RPM。只有一个联动通道。
set 显式进入曲线模式：先写目标，再按需 enable=1；已在曲线时不重复 enable。
没有裸 `mode curve`；进入曲线时必须给出明确目标。
`--wait` 是有限 1–120 秒；`--tolerance` 0–500 RPM（默认 250，仅与 wait 合用）。
两次连续实际 RPM 达到容差才返回 settled=true；否则默认 settled=false，仅表示请求接受。
重复相同目标不重置 EC 闭环；不同目标最多每 15 秒应用一次，小变更另有去抖，应等待实际 RPM 收敛。
等待超时、取消或通信失败均不自动恢复 auto/boost，也不覆盖目标到最高转速。

## 背光

```sh
y720-backlight --json status
y720-backlight --json info             # status 的兼容别名
sudo y720-backlight set --color red --brightness 3
sudo y720-backlight set --colors red blue green white --levels 0 1 2 3
sudo y720-backlight set --effect breath --color blue
sudo y720-backlight off
y720-backlight --json --dry-run apply userspace/examples/backlight-four-zone.json
cat profile.json | sudo y720-backlight --json apply -
```

status/info 只核验 sysfs 身份，不打开 hidraw、不发送任何 ioctl/Feature，不取得写锁。
返回 `device,hid_device,driver,descriptor_sha256,report_bytes,state_known=false,profile=null`。
当前真实配色、亮度、开关无法读回；不读取磁盘缓存冒充硬件状态。
写成功返回 profile、reports_completed=5、transport_accepted=true、visual_verified=false、state_known=false。

effect：heartbeat/breath/smooth/static/wave，默认 static；亮度 0–3，默认 3。
颜色可用 0–19，或 red=0、torch-red=1、pink=2、magenta=3、purple=4、blue=6、cyan=9、green=12、
yellow=15、orange=16、white=18、none=19。不是任意 RGB，更不是逐键 RGB。
`--color` 与 `--colors` 互斥；`--brightness` 与 `--levels` 互斥。
off 提交完整 static 红色/四区亮度 0，不猜测或合并以前的配色。

完整 profile（最多 4096 字节 UTF-8，接受可选 UTF-8 BOM）：

```json
{"schema_version":1,"effect":"static","colors":["red","blue","green","white"],"brightness":[3,3,3,3]}
```

拒绝重复/未知字段、布尔/浮点充当整数、错误数组长度和非法输入。apply FILE 或 `-` 代表 stdin。
dry-run 输出规范化 profile 和 reports_hex，不打开设备/锁或写硬件；不能用于只读 status/info。
五条报告任一步失败、短传输或取消立即停止，报告完整完成数及 state_uncertain，不重试/补交/回滚。

## JSON 与退出码

成功 stdout：`{"schema_version":1,"program":"y720-fan","version":"0.1","ok":true,"data":{...}}`。
失败 stderr：相同公共字段、`ok:false,error:{code,message,details}`；stdout 不混入错误。
错误 details 是可选诊断，字段按阶段变化；message 非稳定解析接口。GUI 依 code/退出码分支。

| 退出码 | 意义 |
|---|---|
| 0 | 查询完成或请求接受；不自动表示硬件达到目标 |
| 2 | 参数/profile错误 |
| 3 | 无匹配/歧义/不支持设备、版本或只读控制接口 |
| 4 | 权限不足 |
| 5 | 协作锁争用，或目标被其他控制者取代 |
| 6 | 写前读取/打开/I/O错误 |
| 7 | 已尝试写入后失败，硬件状态可能部分改变 |
| 8 | 等待实际RPM超时，不回滚 |
| 130/143 | SIGINT/SIGTERM，不额外改硬件 |
| 141 | 输出管道关闭，此前请求不会撤销 |

普通用户权限由部署者决定，CLI 不调用 sudo/pkexec、不设 setuid、不放开所有 hidraw 权限。
两个写锁分别为 /run/y720-fan.lock 与 /run/y720-backlight.lock；只约束配合使用锁的软件。
状态查询无写锁，但不能防止其他程序同时改设备；不要并行运行多个控制者。
