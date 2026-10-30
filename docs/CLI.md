# CLI 0.1.1

`y720-fan` 和 `y720-backlight` 是两个独立 C11 程序，不合并总控，也不常驻。
全局选项 `--json`、`--device NAME`、`--dry-run` 放在命令前。
NAME 只接受 hwmonN 或 hidrawN，不能指定任意路径；手选设备仍需通过完整身份核验。

`--help`、`--version` 不访问硬件。普通写入成功时静默，状态默认输出 key=JSON值，
错误只输出到 stderr。CLI 不自行调用 sudo／pkexec，也不设置 setuid。

## 风扇

```sh
y720-fan --json status
sudo y720-fan mode auto
sudo y720-fan mode boost
sudo y720-fan set --rpm 4001
sudo y720-fan --json set --rpm 4000 --wait 40 --tolerance 250
sudo y720-fan set --pwm 128
printf '1500 1800 2400 3200 4200 5100\n' | sudo y720-fan curve apply -
sudo y720-fan curve stop
y720-fan --json --dry-run set --pwm 0
```

status 是已验证 hwmon 的只读快照，包含：

- `device`、`driver_version`、`controls_available`。
- `mode`：auto／boost／curve；`mode_raw` 为 2／0／1（外部目标）或 3（内核曲线）。
- `target_rpm`、`pwm`：最新请求，不是实测转速。
- `fan_rpm`：两路实际 RPM。
- `diagnostics`：requested_mode、safety_fault、last_error、target_writes、
  mode_writes、ramp_low_samples；缺失项为 null。
- `temperature`：单一控制温度及来源。
- `kernel_curve`：supported、active、engine、error、points、temperature_millidegrees。
  engine 为 linux-thermal 或 null；缺失或 supported=false 时不启用 GUI 内核曲线。

只读加载时控制字段为 null，不解释成 0 或 boost。温度对象格式为：

```json
{"millidegrees":55000,"source":"x86_pkg_temp"}
```

温度单位是毫摄氏度；不可读时 millidegrees 为 null。
本版只提供一个控制温度，不再返回 CPU/GPU 两组字段。source 为 x86_pkg_temp、
coretemp 或 null：优先读取驱动的 temp1_input，兼容显示可回退到内核标准
coretemp 的 Package id 0；这不改变内核曲线的实际温度源。
GUI 显示该温度，不计算曲线。曲线只使用内核 x86_pkg_temp，不依赖 GUI、额外服务、
NVIDIA 驱动或用户态温度写入。多文件读取不是原子快照，短暂不一致时应重读。

set 必须且只能选择 `--rpm 1500..5100` 或 `--pwm 0..255`。
RPM 向上量化到 100 RPM；PWM 是归一化请求，0 对应 1500 RPM，不停转。
两路风扇共用一个目标。

set 先写目标，再按需写 enable=1；已经在外部调速模式时不重复 enable。
从内核曲线写入目标会释放 thermal 控制权，改由外部控制。
不提供裸 `mode curve`，进入曲线必须给出明确目标。
相同目标不重新初始化 EC；不同目标至少间隔 15 秒，100 RPM 小变更还需稳定 10 秒。
实际转速可能在目标改变后短暂下降，应等待收敛，不要密集改写。

`--wait` 接受有限的十进制 1–120 秒；`--tolerance` 为 0–500 RPM，
默认 250，只能与 wait 合用。两次连续观测均在容差内才返回 settled=true。
不等待时 settled=false 仅表示请求已接受，不表示已经达到目标。
等待超时、取消或通信失败不自动恢复 auto／boost，也不覆盖为固定全速。

`curve apply -` 从 stdin 接收六个非递减整百 RPM，依次对应 40/50/60/70/80/90°C。
范围 1500–5100，支持整条 5100，不以过热为启用条件；仅支持 stdin，不接受文件路径。
完整校验后一次写入驱动，Linux thermal 负责后续采样、1°C 选档和 2°C 降温滞回，
CLI/GUI 退出不停止。驱动还会按原有传输限频提交目标，实际转速不是瞬时变化。
首次提交要求必要温度有效；运行中缺失时保留最后目标并重试。
`curve stop` 停止内核选档、保留当前软件目标，不切 auto/boost；重复 stop 不写硬件。
能力要求为 `control_api=2`、`curve_backend=linux-thermal`、
`temperature_source=x86_pkg_temp` 和同版驱动，不能只判断版本号。

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

status／info 仅核验 sysfs 身份，不打开 hidraw、不发送 ioctl／Feature，也不申请写锁。
返回 device、hid_device、driver、descriptor_sha256、report_bytes，
以及 `state_known=false`、`profile=null`。磁盘缓存不作为硬件状态。

写入成功返回提交的 profile、reports_completed=5、transport_accepted=true、
visual_verified=false、state_known=false；传输成功不是视觉验证。

效果支持 heartbeat／breath／smooth／static／wave，默认 static；亮度 0–3，默认 3。
颜色支持 0–19，或以下名称：red=0、torch-red=1、pink=2、magenta=3、purple=4、
blue=6、cyan=9、green=12、yellow=15、orange=16、white=18、none=19。
这是固定调色板和四区控制，不是任意 RGB 或逐键 RGB。

`--color` 与 `--colors` 互斥，`--brightness` 与 `--levels` 互斥。
off 提交 static 红色、四区亮度 0 的完整配置，不猜测以前的配色。

完整 profile 最大 4096 字节 UTF-8，可带 UTF-8 BOM：

```json
{"schema_version":1,"effect":"static","colors":["red","blue","green","white"],"brightness":[3,3,3,3]}
```

apply FILE 从文件读取，apply - 从 stdin 读取。拒绝未知或重复字段、错误数组长度、
越界值以及用浮点或布尔值代替整数。dry-run 输出规范化配置和 reports_hex，
不打开硬件或锁，不能与只读 status／info 合用。

五条报告任一步失败、短传输或取消时立即停止，返回已完整发送的数量及
state_uncertain，不重试、不补交、不恢复旧配置。

## JSON 与退出码

成功向 stdout 输出一个 JSON 对象：

```json
{"schema_version":1,"program":"y720-fan","version":"0.1.1","ok":true,"data":{}}
```

data 的实际字段依命令而定。失败向 stderr 输出相同公共字段及
`ok:false,error:{code,message,details}`，不向 stdout 混入错误。
details 可缺省或扩展；message 供人阅读，程序应依 code 和退出码分支。

| 退出码 | 含义 |
|---|---|
| 0 | 查询完成或请求接受，不代表已经达到转速 |
| 2 | 参数或 profile 错误 |
| 3 | 设备不匹配、存在歧义、版本不支持或控制接口只读 |
| 4 | 权限不足 |
| 5 | 协作锁争用或请求被其他控制者取代 |
| 6 | 写入前的读取、打开或 I/O 错误 |
| 7 | 尝试写入后失败，硬件状态可能已改变 |
| 8 | 等待实际 RPM 超时，不回滚 |
| 130／143 | SIGINT／SIGTERM，不额外改写硬件 |
| 141 | 输出管道关闭，此前请求不会撤销 |

写锁分别为 `/run/y720-fan.lock`、`/run/y720-backlight.lock`，
只约束采用相同锁的软件。普通用户的权限由部署者决定；
项目不放开所有 hidraw，也不提供任意硬件路径覆盖。
