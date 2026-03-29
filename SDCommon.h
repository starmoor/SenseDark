// SenseDark 跨进程共享常量：SB 侧（Tweak.x）与 Preferences 侧（SenseDarkSettings.x / SenseDarkPrefs.m）
// 靠这些字符串对上协议——域名/通知名/键名/默认值/范围散落多处一旦手改漂移即静默失联
// （改了名另一边读不到、通知收不着、默认值对不齐），故集中唯一份。
// 用法：Objective-C 侧 @SDKEY_xxx / @SDDEF_xxx，CoreFoundation 侧 CFSTR(SD_PREFS_DOMAIN)。

#pragma once

// 偏好存储与跨进程通知（域名对齐 GitHub 用户名 starmoor）
#define SD_PREFS_DOMAIN       "com.starmoor.sensedark"
#define SD_PREFS_PATH         "/var/mobile/Library/Preferences/com.starmoor.sensedark.plist"
#define SD_NOTE_PREFS_CHANGED "com.starmoor.sensedark.prefschanged" // Settings 写偏好后广播，SB 侧热重载
#define SD_NOTE_LUX           "com.starmoor.sensedark.lux"          // SB 每次采样发布 notify state（lux+1，0=尚未上报）
#define SD_NOTE_STATE         "com.starmoor.sensedark.state"        // SB 运行状态位掩码（零 I/O）
#define SD_NOTE_RESUME        "com.starmoor.sensedark.resume"       // Settings 点「立即恢复」通知 SB 解除手动暂停
#define SD_DEBUG_DOMAIN       SD_PREFS_DOMAIN ".debug"              // 诊断日志域（SDLog.h 写）
#define SD_GITHUB_URL         "https://github.com/starmoor/SenseDark"

// 版本号比较（"0.1.0-41" 风格）：按非数字分段逐段比大小，段缺按 0。
// 本项目版本全数字段，够用；需在 Foundation 之后 include。
static inline int SDVersionCompare(NSString *a, NSString *b)
{
    if (a.length == 0) return b.length ? -1 : 0;
    if (b.length == 0) return 1;
    NSCharacterSet *nonDigit = [[NSCharacterSet decimalDigitCharacterSet] invertedSet];
    NSArray<NSString *> *sa = [a componentsSeparatedByCharactersInSet:nonDigit];
    NSArray<NSString *> *sb = [b componentsSeparatedByCharactersInSet:nonDigit];
    NSUInteger n = sa.count > sb.count ? sa.count : sb.count;
    for (NSUInteger i = 0; i < n; i++) {
        long long va = i < sa.count ? [sa[i] longLongValue] : 0;
        long long vb = i < sb.count ? [sb[i] longLongValue] : 0;
        if (va != vb) return va < vb ? -1 : 1;
    }
    return 0;
}

// SD_NOTE_STATE 位掩码
#define SDSTATE_PAUSED_MANUAL (1ULL << 0) // 手动切换暂停中
#define SDSTATE_PAUSED_LP     (1ULL << 1) // 低电量暂停中
#define SDSTATE_SLOW_LP       (1ULL << 2) // 低电量降频中

// 偏好键
#define SDKEY_ENABLED     "enabled"          // 总闸：选项页选"根据环境光"=1，"自动"开关关=0
#define SDKEY_UP          "upLux"            // lux >= upLux 切浅色
#define SDKEY_DOWN        "downLux"          // lux <= downLux 切深色
#define SDKEY_INTERVAL    "interval"         // 采样间隔（秒）
#define SDKEY_DELAY       "switchDelay"      // 越限防抖（秒）
#define SDKEY_PAUSE       "pauseOnManual"    // 手动切换后暂停自动
#define SDKEY_SCHEDMODE   "scheduleMode"     // 0=面板开关 1=日落 2=自定义 3=环境光接管
#define SDKEY_AUTOON      "automaticOn"      // 系统"自动"开关镜像（Settings 侧写）
#define SDKEY_LP_POLICY   "lowPowerPolicy"   // 低电量策略：0=保持不变 1=降频采样 2=暂停切换
#define SDKEY_LP_INTERVAL "lowPowerInterval" // 低电量降频采样间隔（秒）
#define SDKEY_LANG        "language"         // 界面语言："system" / "zh" / "en" / "ru"

// 低电量策略枚举（与 Android 端 PowerSavePolicy 对齐）
#define SDLP_KEEP  0
#define SDLP_SLOW  1
#define SDLP_PAUSE 2

// 参数范围（与 Android 端 SenseDarkPrefs.kt 对齐；阈值支持小数，最小滞回差 0.1 lux）
#define SDMIN_UP          1.0
#define SDMAX_UP          300.0
#define SDMIN_DOWN        0.0
#define SDMAX_DOWN        50.0
#define SDMIN_LUX_GAP     0.1
#define SDMIN_DELAY       0
#define SDMAX_DELAY       15
#define SDMIN_INTERVAL    1
#define SDMAX_INTERVAL    15
#define SDMIN_LP_INTERVAL 10
#define SDMAX_LP_INTERVAL 60

// 默认值（SB 侧 ApplyPrefsDict 兜底 / Settings 侧滑块构造与读取回退，三处同源）
#define SDDEF_ENABLED     1
#define SDDEF_UP          25
#define SDDEF_DOWN        10
#define SDDEF_INTERVAL    3
#define SDDEF_DELAY       2
#define SDDEF_PAUSE       1
#define SDDEF_LP_POLICY   SDLP_KEEP
#define SDDEF_LP_INTERVAL 30
