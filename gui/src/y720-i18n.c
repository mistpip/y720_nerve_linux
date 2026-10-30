/* SPDX-License-Identifier: MIT */
#include "y720-i18n.h"
#include <adwaita.h>

#include <string.h>

static gboolean language_is_english(const gchar *value)
{
    if (!value || !*value)
        return FALSE;
    while (*value) {
        if (g_ascii_strncasecmp(value, "en", 2) == 0 &&
            (value[2] == '\0' || value[2] == '_' || value[2] == '.' ||
             value[2] == ':' || value[2] == '@'))
            return TRUE;
        if (g_ascii_strncasecmp(value, "zh", 2) == 0 &&
            (value[2] == '\0' || value[2] == '_' || value[2] == '.' ||
             value[2] == ':' || value[2] == '@'))
            return FALSE;
        value = strchr(value, ':');
        if (!value)
            break;
        value++;
    }
    return FALSE;
}

static gchar *forced_language;

void y720_i18n_prepare_environment(const gchar *language)
{
    const gchar *selected = language;
    if (!selected || !*selected || !g_strcmp0(selected, "auto"))
        selected = g_getenv("Y720_LANG");
    if (selected && g_str_has_prefix(selected, "en"))
        g_setenv("LANGUAGE", "en", TRUE);
    else if (selected && g_str_has_prefix(selected, "zh"))
        g_setenv("LANGUAGE", "zh_CN", TRUE);
}

void y720_i18n_set_language(const gchar *language)
{
    g_free(forced_language);
    forced_language = (language && *language && g_strcmp0(language, "auto") != 0)
                          ? g_strdup(language) : NULL;
}

gboolean y720_i18n_english(void)
{
    if (forced_language)
        return g_str_has_prefix(forced_language, "en");
    const gchar *override = g_getenv("Y720_LANG");
    if (override && *override)
        return language_is_english(override);

    const gchar *language = g_getenv("LANGUAGE");
    if (language && *language)
        return language_is_english(language);

    const gchar *locale = g_getenv("LC_ALL");
    if (!locale || !*locale)
        locale = g_getenv("LC_MESSAGES");
    if (!locale || !*locale)
        locale = g_getenv("LANG");

    if (locale && *locale) {
        if (g_ascii_strncasecmp(locale, "en", 2) == 0)
            return TRUE;
        if (g_ascii_strncasecmp(locale, "zh", 2) == 0)
            return FALSE;
    }

    const gchar *const *names = g_get_language_names();
    for (guint i = 0; names && names[i]; i++) {
        if (strcmp(names[i], "C") == 0 || strcmp(names[i], "POSIX") == 0)
            continue;
        if (g_str_has_prefix(names[i], "en"))
            return TRUE;
        if (g_str_has_prefix(names[i], "zh"))
            return FALSE;
    }
    return FALSE;
}

const gchar *y720_tr(const gchar *text)
{
    if (!text || !y720_i18n_english())
        return text;

    static const struct { const gchar *zh; const gchar *en; } strings[] = {
        {"Wayland 需要按键名称，请重新录制旧的 X11 键码", "Wayland requires named keys; record legacy X11 keycodes again"},
        {"由桌面分配", "Assigned by the desktop"},
        {"Wayland 桌面快捷键已启用", "Wayland desktop shortcuts enabled"},
        {"Wayland 未授权全部快捷键", "Not all Wayland shortcuts were authorized"},
        {"Wayland 快捷键授权已取消", "Wayland shortcut authorization cancelled"},
        {"Wayland 桌面无法注册快捷键", "The Wayland desktop could not register shortcuts"},
        {"Wayland 桌面返回了无效的快捷键会话", "The Wayland desktop returned an invalid shortcut session"},
        {"Wayland 桌面未返回快捷键授权结果", "The Wayland desktop did not return shortcut authorization results"},
        {"Wayland 快捷键不可用：%s", "Wayland shortcuts unavailable: %s"},
        {"Wayland 快捷键会话已关闭，请重新启用", "Wayland shortcut session closed; enable shortcuts again"},
        {"Wayland 快捷键 Portal 不可用，请检查桌面后端", "Wayland shortcut portal unavailable; check the desktop backend"},
        {"无法连接桌面会话总线", "Cannot connect to the desktop session bus"},
        {"正在等待 Wayland 桌面授权快捷键…", "Waiting for Wayland desktop shortcut authorization…"},
        {"X11 全局组合键已启用", "X11 global key combinations enabled"},
        {"普通组合键未注册，检查桌面支持或快捷键占用", "Key combinations not registered; check desktop support or shortcut conflicts"},
        {"切换 auto/boost", "Toggle auto/boost"},
        {"全局快捷键已关闭", "Global shortcuts disabled"},
        {"Y720 专用功能键可用", "Y720 hardware shortcuts available"},
        {"Y720 专用功能键不可读，请检查活动会话与输入权限", "Cannot read Y720 hardware shortcuts; check the active session and input permissions"},
        {"主菜单", "Main menu"},
        {"自定义", "Custom"},
        {"刷新状态", "Refresh status"},
        {"图片", "Images"},
        {"清除背景", "Clear background"},
        {"按下新的快捷键", "Press a new shortcut"},
        {"X11 组合键与 Y720 输入事件", "X11 shortcuts and Y720 input events"},
        {"检查输入设备权限或快捷键占用", "Check input permissions or shortcut conflicts"},
        {"无法读取 Fn+空格，请检查输入设备权限", "Cannot read Fn+Space; check input device permissions"},
        {"小键盘 7（NumLock 关闭）", "Keypad 7 (NumLock off)"},
        {"键码 %s", "Keycode %s"},
        {"方案 %d", "Profile %d"},
        {"已提交，未读回", "Submitted; hardware state cannot be read back"},
        {"正在读取设备…", "Reading device…"},
        {"正在请求授权…", "Requesting authorization…"},
        {"已发送 %d/5 条%s", "%d/5 reports sent%s"},
        {"，状态不确定", "; hardware state uncertain"},
        {"未找到 y720-backlight", "y720-backlight was not found"},
        {"背光回执未确认五条报告已提交，硬件状态未知", "Backlight reply did not confirm all five reports; hardware state unknown"},
        {"内核曲线温度暂不可用，驱动保留最后目标并重试", "Kernel curve temperature unavailable; driver retains the last target and retries"},
        {"当前驱动不支持内核曲线，请安装支持 control_api=2 的驱动", "This driver does not support kernel curves; install a driver with control_api=2"},
        {"驱动与 GUI 版本不一致，已禁用写入", "Driver and GUI versions differ; writes disabled"},
        {"写入回执缺少有效的同版控制状态", "Write reply lacks valid control status from a matching driver"},
        {"驱动以只读方式加载，未开放控制接口。", "Driver loaded read-only; control interfaces are disabled."},
        {"另一个控制程序占用了设备。", "Another controller is using the device."},
        {"写入可能只完成了一部分，请重新读取状态。", "Write may be partial; read status again."},
        {"等待转速收敛超时，目标仍然保留。", "Timed out waiting for RPM to settle; the target is retained."},
        {"模式或目标已被其他程序改写。", "Another controller changed the mode or target."},
        {"驱动版本不支持写入。", "Driver version or capabilities do not support this operation."},
        {"读写驱动接口失败。", "Failed to access the driver interface."},
        {"CLI 输出格式不符合预期。", "Unexpected CLI output format."},
        {"操作已取消", "Operation cancelled"},
        {"操作已取消；硬件状态可能已经改变", "Operation cancelled; hardware state may already have changed"},
        {"CLI 在规定时间内没有完成", "CLI timed out"},
        {"CLI 在规定时间内没有完成，已请求终止", "CLI timed out; termination requested"},
        {"CLI 没有返回任何 JSON 输出", "CLI returned no JSON output"},
        {"CLI 输出不是合法 JSON：%s", "Invalid CLI JSON: %s"},
        {"CLI 输出 JSON 不是对象", "CLI JSON root is not an object"},
        {"CLI 输出的 schema_version 不是 1", "CLI schema_version is not 1"},
        {"CLI 输出缺少 program 或 version", "CLI output lacks program or version"},
        {"期望 %s，实际 %s", "Expected %s, received %s"},
        {"CLI 与 GUI 版本不一致，请安装同版组件", "CLI and GUI versions differ; install matching components"},
        {"CLI 输出缺少布尔字段 ok", "CLI output lacks boolean field ok"},
        {"CLI 退出码为 0 但 ok=false", "CLI exited successfully but reported ok=false"},
        {"CLI 成功输出缺少 data 对象", "Successful CLI reply lacks data object"},
        {"CLI 失败退出但 ok=true", "CLI exited with an error but reported ok=true"},
        {"CLI 失败但没有错误信息", "CLI failed without an error message"},
        {"CLI 被信号终止", "CLI terminated by a signal"},
        {"与 CLI 通信失败", "Communication with CLI failed"},
        {"无法启动 CLI", "Could not start CLI"},
        {"拒绝提权运行未核验的 CLI 路径", "Refusing privilege escalation for an unverified CLI path"},
        {"字段 %s 缺失或不是布尔值", "Field %s is missing or not boolean"},
        {"字段 %s 缺失", "Field %s is missing"},
        {"字段 %s 不能为 null", "Field %s cannot be null"},
        {"字段 %s 不是字符串", "Field %s is not a string"},
        {"字段 %s 不是整数", "Field %s is not an integer"},
        {"字段 %s 不是数组", "Field %s is not an array"},
        {"字段 %s 需要 4 个元素", "Field %s needs four elements"},
        {"字段 %s[%u] 不是整数", "Field %s[%u] is not an integer"},
        {"诊断字段 %s 不是整数", "Diagnostic field %s is not an integer"},
        {"四区配置数值超出范围", "Zone setting is out of range"},
        {"风扇 status 缺少 data 对象", "Fan status lacks data object"},
        {"字段 fan_rpm 不是数组", "fan_rpm is not an array"},
        {"字段 fan_rpm 需要 2 个元素", "fan_rpm needs two elements"},
        {"字段 fan_rpm 包含非整数", "fan_rpm contains a non-integer"},
        {"字段 diagnostics 不是对象", "diagnostics is not an object"},
        {"风扇控制状态超出范围或模式不一致", "Fan control state is out of range or inconsistent"},
        {"实际转速超出有效范围", "Measured RPM is out of range"},
        {"temperature 不是对象", "temperature is not an object"},
        {"内核曲线状态无效", "Invalid kernel curve state"},
        {"内核曲线需要六个转速点", "Kernel curve needs six RPM points"},
        {"内核曲线转速无效", "Invalid kernel curve RPM"},
        {"内核曲线转速超出范围", "Kernel curve RPM is out of range"},
        {"背光 profile 不是对象", "Backlight profile is not an object"},
        {"背光效果无效", "Invalid backlight effect"},
        {"背光 status 缺少 data 对象", "Backlight status lacks data object"},
        {"背光报告长度不受支持", "Unsupported backlight report length"},
        {"字段 profile 缺失", "profile is missing"},
        {"字段 profile 既不是对象也不是 null", "profile is neither an object nor null"},
        {"无法创建目录 %s：%s", "Cannot create directory %s: %s"},
        {"自启动文件已存在且不是本程序创建的，未删除：%s", "Not removing an autostart file created by another program: %s"},
        {"自启动文件已存在且不是本程序创建的，未覆盖：%s", "Not replacing an autostart file created by another program: %s"},
        {"无法删除自启动文件：%s", "Cannot remove autostart file: %s"},
        {"无法确定程序路径", "Cannot determine executable path"},
        {"没有找到 Y720 键盘输入节点", "Y720 keyboard input node not found"},
        {"无法读取 %s：%s", "Cannot read %s: %s"},
        {"键盘身份已改变", "Keyboard identity changed"},
        {"evdev 键码无效", "Invalid evdev keycode"},
        {"没有读取 %s 的权限", "Permission denied reading %s"},
        {"全局快捷键仅在 X11 会话可用", "Global key combinations require an X11 session"},
        {"快捷键数量无效", "Invalid shortcut count"},
        {"无法打开 X11 显示", "Cannot open X11 display"},
        {"XWayland 会话无法提供全局快捷键", "Global shortcuts are unavailable through XWayland"},
        {"键码无效", "Invalid keycode"},
        {"无法解析快捷键", "Cannot parse shortcut"},
        {"无法解析键码", "Cannot resolve keycode"},
        {"两个快捷键重复", "Duplicate shortcuts"},
        {"快捷键已被其他程序占用", "Shortcut is already used by another program"},
        {"构建时未启用 X11", "X11 support was not enabled at build time"},
        {"极致散热", "Extreme cooling"},
        {"幻彩地带", "Keyboard light"},
        {"风扇", "Fan"},
        {"灯光", "Light"},
        {"状态", "Status"},
        {"控制", "Controls"},
        {"模式", "Mode"},
        {"自动", "Automatic"},
        {"手动", "Manual"},
        {"曲线", "Curve"},
        {"风扇曲线", "Fan curve"},
        {"手动转速", "Manual speed"},
        {"转速", "Speed"},
        {"设备", "Device"},
        {"等待状态", "Waiting"},
        {"只读", "Read-only"},
        {"刷新", "Refresh"},
        {"应用", "Apply"},
        {"保存", "Save"},
        {"恢复默认", "Reset"},
        {"设置", "Settings"},
        {"关于", "About"},
        {"退出", "Quit"},
        {"启动", "Startup"},
        {"开机自动启动", "Start automatically"},
        {"启动时隐藏窗口", "Start hidden"},
        {"关闭窗口后留在托盘", "Keep running in tray"},
        {"快捷键", "Shortcuts"},
        {"切换灯光方案", "Next light profile"},
        {"启用全局快捷键", "Enable global shortcuts"},
        {"启用风扇曲线", "Enable fan curve"},
        {"外观", "Appearance"},
        {"背景图片", "Background image"},
        {"选择…", "Choose…"},
        {"背景不透明度", "Background opacity"},
        {"面板不透明度", "Panel opacity"},
        {"窗口不透明度", "Window opacity"},
        {"方案", "Profiles"},
        {"配置文件", "Profile"},
        {"语言", "Language"},
        {"跟随系统", "System"},
        {"重启程序后生效", "Restart to apply"},
        {"切换方案", "Switch profile"},
        {"方案 1", "Profile 1"},
        {"方案 2", "Profile 2"},
        {"方案 3", "Profile 3"},
        {"不开灯", "Lights off"},
        {"颜色 / 亮度", "Color / brightness"},
        {"灯效", "Effect"},
        {"常亮", "Static"},
        {"呼吸", "Breath"},
        {"心跳", "Heartbeat"},
        {"平滑", "Smooth"},
        {"波浪", "Wave"},
        {"温度", "Temperature"},
        {"驱动报告散热异常，曲线目标保持不变；详情请查看 CLI 状态", "Driver reports a cooling fault; curve target unchanged. Check CLI status for details"},
        {"内核曲线后端或控制状态不一致", "Inconsistent kernel curve backend or control state"},
        {"内核曲线已停止，控制权已交给其他模式或外部控制者", "Kernel curve stopped; another mode or external controller has control"},
        {"曲线转速必须按温度递增或保持不变，步长为 100 RPM", "Curve RPM must be nondecreasing with temperature, in steps of 100 RPM"},
        {"温度读数不可用", "Temperature readings unavailable"},
        {"控制温度来源无效", "Invalid control temperature source"},
        {"内核控制温度，来源为 CPU 封装传感器", "Kernel control temperature from the CPU package sensor"},
        {"曲线目标", "Curve target"},
        {"未选择", "Not selected"},
        {"未设置", "Not set"},
        {"键码", "Keycode"},
        {"空格", "Space"},
        {"重命名方案", "Rename profile"},
        {"方案名称", "Profile name"},
        {"取消", "Cancel"},
        {"重命名", "Rename"},
        {"删除方案", "Delete profile"},
        {"新建方案", "New profile"},
        {"点击键盘选择区域", "Click a keyboard zone"},
        {"拖动节点调整温度对应的目标转速", "Drag points to map temperature to fan speed"},
        {"没有检测到 Y720 硬件。", "No Y720 hardware detected."},
        {"需要管理员权限。", "Administrator permission is required."},
        {"操作已取消。", "Operation cancelled."},
        {"写入失败", "Write failed"},
        {"无法启动 CLI。", "Could not start the CLI."},
        {"窗口已隐藏", "Window hidden"},
        {"仅 X11 会话可用", "X11 session only"},
        {"X11 全局", "X11 global"},
        {"快捷键被占用", "Shortcut already in use"},
        {"桌面环境不支持托盘", "Tray is not supported"},
        {"登录后启动", "Start after login"},
        {"启动后只显示托盘图标", "Start with tray icon only"},
        {"关于 Y720 Nerve Center", "About Y720 Nerve Center"},
        {"风扇与键盘背光控制。", "Fan and keyboard backlight control."},
        {"选择背景图片", "Choose background image"},
        {"窗口已显示", "Window shown"},
        {"隐藏窗口", "Hide window"},
        {"显示窗口", "Show window"},
        {"自动模式", "Automatic mode"},
        {"曲线模式", "Curve mode"},
        {"设置…", "Settings…"},
        {"已关闭开机启动", "Autostart disabled"},
        {"已设置开机启动", "Autostart enabled"},
        {"方案已应用", "Profile applied"},
        {"方案已重命名", "Profile renamed"},
        {"请求已提交", "Request sent"},
        {"已开启极致散热", "Extreme cooling enabled"},
        {"已切换为自动模式", "Switched to automatic"},
        {"自定义转速已保存", "Manual speed saved"},
        {"已保存并观察到短时收敛", "Saved; RPM settled briefly"},
        {"只读模式：allow_control=1 后可控制", "Read-only; enable allow_control to change settings"},
        {"无法读取风扇状态", "Could not read fan status"},
        {"无法读取背光状态", "Could not read backlight status"},
        {"CLI 成功输出缺少 data", "CLI response is missing data"},
        {"未找到 y720-fan；请安装或设置 Y720_FAN_BIN", "y720-fan not found; install it or set Y720_FAN_BIN"},
        {"需要管理员权限", "Administrator permission is required"},
        {"由固件控制", "Controlled by firmware"},
        {"风扇 1", "Fan 1"},
        {"风扇 2", "Fan 2"},
        {"红", "Red"},
        {"火炬红", "Torch red"},
        {"粉", "Pink"},
        {"洋红", "Magenta"},
        {"紫", "Purple"},
        {"灰", "Gray"},
        {"蓝", "Blue"},
        {"深蓝", "Dark blue"},
        {"浅蓝", "Light blue"},
        {"青", "Cyan"},
        {"深绿", "Dark green"},
        {"亮绿", "Bright green"},
        {"绿", "Green"},
        {"淡绿", "Pale green"},
        {"橄榄", "Olive"},
        {"黄", "Yellow"},
        {"橙", "Orange"},
        {"深橙", "Dark orange"},
        {"白", "White"},
        {"无", "None"},
        {"颜色 %d", "Color %d"},
        {"Fn+空格", "Fn+Space"},
    };

    for (guint i = 0; i < G_N_ELEMENTS(strings); i++) {
        if (g_strcmp0(text, strings[i].zh) == 0)
            return strings[i].en;
    }
    return text;
}

static void apply_one(GtkWidget *widget)
{
    if (ADW_IS_PREFERENCES_PAGE(widget)) {
        const gchar *title = adw_preferences_page_get_title(ADW_PREFERENCES_PAGE(widget));
        if (title)
            adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(widget), y720_tr(title));
    }
    if (GTK_IS_LABEL(widget)) {
        const gchar *text = gtk_label_get_text(GTK_LABEL(widget));
        const gchar *translated = y720_tr(text);
        if (translated != text)
            gtk_label_set_text(GTK_LABEL(widget), translated);
    }
    if (GTK_IS_BUTTON(widget)) {
        const gchar *text = gtk_button_get_label(GTK_BUTTON(widget));
        if (text) {
            const gchar *translated = y720_tr(text);
            if (translated != text)
                gtk_button_set_label(GTK_BUTTON(widget), translated);
        }
    }
    const gchar *tooltip = gtk_widget_get_tooltip_text(widget);
    if (tooltip) {
        const gchar *translated = y720_tr(tooltip);
        if (translated != tooltip)
            gtk_widget_set_tooltip_text(widget, translated);
    }
    if (ADW_IS_DIALOG(widget)) {
        const gchar *title = adw_dialog_get_title(ADW_DIALOG(widget));
        if (title) {
            const gchar *translated = y720_tr(title);
            if (translated != title)
                adw_dialog_set_title(ADW_DIALOG(widget), translated);
        }
    }
    if (ADW_IS_BANNER(widget)) {
        const gchar *title = adw_banner_get_title(ADW_BANNER(widget));
        if (title) {
            const gchar *translated = y720_tr(title);
            if (translated != title)
                adw_banner_set_title(ADW_BANNER(widget), translated);
        }
    }
    if (ADW_IS_PREFERENCES_GROUP(widget)) {
        const gchar *title = adw_preferences_group_get_title(ADW_PREFERENCES_GROUP(widget));
        if (title) {
            const gchar *translated = y720_tr(title);
            if (translated != title)
                adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(widget), translated);
        }
        const gchar *description = adw_preferences_group_get_description(ADW_PREFERENCES_GROUP(widget));
        if (description)
            adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(widget), y720_tr(description));
    }
    if (GTK_IS_WINDOW(widget)) {
        const gchar *title = gtk_window_get_title(GTK_WINDOW(widget));
        if (title) {
            const gchar *translated = y720_tr(title);
            if (translated != title)
                gtk_window_set_title(GTK_WINDOW(widget), translated);
        }
    }
    if (ADW_IS_PREFERENCES_ROW(widget)) {
        const gchar *title = adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget));
        if (title) {
            const gchar *translated = y720_tr(title);
            if (translated != title)
                adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widget), translated);
        }
        if (!ADW_IS_ACTION_ROW(widget))
            return;
        const gchar *subtitle = adw_action_row_get_subtitle(ADW_ACTION_ROW(widget));
        if (subtitle) {
            const gchar *translated = y720_tr(subtitle);
            if (translated != subtitle)
                adw_action_row_set_subtitle(ADW_ACTION_ROW(widget), translated);
        }
    }
}

void y720_i18n_apply(GtkWidget *root)
{
    if (!root || !y720_i18n_english() || g_object_get_data(G_OBJECT(root), "y720-i18n-user-text"))
        return;
    apply_one(root);
    for (GtkWidget *child = gtk_widget_get_first_child(root);
         child; child = gtk_widget_get_next_sibling(child))
        y720_i18n_apply(child);
}
