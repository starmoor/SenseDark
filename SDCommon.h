// SenseDark 跨进程共享常量：SB 侧（Tweak.x）与 Preferences 侧（SenseDarkSettings.x）
// 靠这些字符串对上协议——域名/通知名/键名/默认值散落两处一旦手改漂移即静默失联
// （改了名另一边读不到、通知收不着、默认值对不齐），故集中唯一份。
// 用法：Objective-C 侧 @SDKEY_xxx / @SDDEF_xxx，CoreFoundation 侧 CFSTR(SD_PREFS_DOMAIN)。

#pragma once

// 偏好存储与跨进程通知（域名对齐 GitHub 用户名 starmoor）
#define SD_PREFS_DOMAIN       "com.starmoor.sensedark"
#define SD_PREFS_PATH         "/var/mobile/Library/Preferences/com.starmoor.sensedark.plist"
#define SD_NOTE_PREFS_CHANGED "com.starmoor.sensedark.prefschanged" // Settings 写偏好后广播，SB 侧热重载
#define SD_NOTE_LUX           "com.starmoor.sensedark.lux"          // SB 每次采样发布 notify state（lux+1，0=尚未上报）
#define SD_DEBUG_DOMAIN       SD_PREFS_DOMAIN ".debug"              // 诊断日志域（SDLog.h 写）

// 偏好键
#define SDKEY_ENABLED   "enabled"       // 总闸：选项页选"根据环境光"=1，"自动"开关关=0
#define SDKEY_UP        "upLux"         // lux >= upLux 切浅色
#define SDKEY_DOWN      "downLux"       // lux <= downLux 切深色
#define SDKEY_INTERVAL  "interval"      // 采样间隔（秒）
#define SDKEY_DELAY     "switchDelay"   // 越限防抖（秒）
#define SDKEY_PAUSE     "pauseOnManual" // 手动切换后暂停自动
#define SDKEY_SCHEDMODE "scheduleMode"  // 0=面板开关 1=日落 2=自定义 3=环境光接管
#define SDKEY_AUTOON    "automaticOn"   // 系统"自动"开关镜像（Settings 侧写）

// 默认值（SB 侧 ApplyPrefsDict 兜底 / Settings 侧滑块构造与读取回退，三处同源）
#define SDDEF_ENABLED  1
#define SDDEF_UP       25
#define SDDEF_DOWN     10
#define SDDEF_INTERVAL 3
#define SDDEF_DELAY    2
#define SDDEF_PAUSE    1
