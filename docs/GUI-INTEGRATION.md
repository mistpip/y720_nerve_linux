# GUI 集成契约 · 0.1

两个后端分别调用，使用 argv 数组和 shell=false；不把 GUI 整体以 root 运行。
固定请求 `--json` 并同时捕获 stdout/stderr/退出码，验证 schema_version=1、program、version 和 ok。
成功一个 JSON 到 stdout，失败一个 JSON 到 stderr。message 可变，details 可缺字段；依 code/退出码处理。
进程无法启动、JSON 无效或无输出也必须显示错误，不能推定操作成功。

## 查询当前状态

```python
subprocess.run(["/usr/local/bin/y720-fan", "--json", "status"],
               capture_output=True, text=True, timeout=5, check=False)
subprocess.run(["/usr/local/bin/y720-backlight", "--json", "status"],
               capture_output=True, text=True, timeout=5, check=False)
```

示例 Python 仅是 GUI 调用伪例，CLI 不依赖 Python。字段详见 CLI.md。
风扇约 1 秒查询一次；分别显示最新请求和两个实际 RPM，不用 PWM 推算测量值。
控制字段 null 表示只读/不可用，不解释成 boost/0 RPM；诊断存在不代表驱动已实施全速保护。
多文件状态不是原子快照，短暂不一致应重读。禁止每次查询重复写目标/enable。

背光 status 不读取真实颜色/亮度/开关：始终 state_known=false、profile=null。
UI 显示“当前硬件配色未知”，可另外显示“用户保存的期望配置”，不可混称当前状态。
即使刚刚写成功，也只能标注“配置已提交，未读回”，而不能改称硬件读回成功。
若未来实现可靠读回，另行扩展能力字段/契约；现有 status 不从缓存猜测。

## 写入

风扇 set 显式进入 curve，mode auto/boost 只能由用户选择。曲线滑块 1500–5100，最高值随时可选。
settled=false 仅接受请求；wait 的 settled=true 也仅代表两次短时容差内观测，不是长期热安全保证。
GUI 若执行温度曲线，需单独设计 CPU/GPU 采样、异常策略和单一控制者，CLI 不自带曲线后台。
温度、通信失败或退出 GUI 都不能擅自发 auto、boost 或固定全速。

背光编辑器维护完整四区 profile，去抖后通过 `apply -` 的 stdin 提交。
不用未知硬件状态合并局部配置，不逐帧调用 CLI；profile 格式见 CLI.md。
reports_completed=5 / transport_accepted=true 不是视觉确认；失败时已接受部分报告也可能改变设备。
显示不确定状态，不补交/重试/恢复旧缓存；由用户明确重新应用。

## 权限与取消

GUI 普通用户运行；未来提权代理只允许固定 root 所有且不可由用户改写的可执行路径及限定操作。
清理 LD_PRELOAD/LD_LIBRARY_PATH 等环境注入入口；不提权运行 ~/.local 中可改写的程序。
代理优先只允许背光 apply -，不开放以 root 读取任意 profile 文件或执行任意 shell。
所有调用在工作线程，有界超时；取消先 SIGTERM 并等待，再按 UI 策略处理。
超时、强杀、管道关闭均不保证没写入；不隐式回滚。风扇另调 status；背光仍显示未知。
两个 CLI 无长期 IPC/D-Bus/心跳；后续代理协议需单独版本化。

GUI 测试至少覆盖：成功、只读、设备消失、权限、竞争、部分写、超时、取消、未知 schema、无效 JSON。
状态测试必须验证背光不伪造读回和所有错误分支不偷偷写 auto/boost/灯光。
