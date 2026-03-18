#import <UIKit/UIKit.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <substrate.h>
#include <notify.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

// SenseDark（SB 侧）：环境光 → 系统深浅色自动切换
// 机制参考 skittyblock/LightsOut(GPLv3)，实现全部重写：
//   - ALS 订阅：见 SDALSClient.h（IOHIDEventSystemClient 匹配 0xff00/4，dlsym 不链私有 tbd）
//   - 切换：UIKit 私有类 UISUserInterfaceStyleMode setModeValue:(1=浅 2=深)；
//     原生"自动"在场时走 setOverride: 保持开关不被系统弹回
//   - 滞回：lux >= upLux 切浅、lux <= downLux 切深（upLux > downLux，防临界抖动）
//   - 低电量策略：监听 isLowPowerModeEnabled，支持保持不变 / 降频采样 / 自动暂停
//   - 手动切换（CC/设置）→ 暂停自动，解锁恢复；锁屏/灭屏 → 停传感器，解锁恢复
// 只注入 SpringBoard（SenseDark.plist）。Preferences 侧见 SenseDarkSettings.x。

#define SD_DIAG 0         // 诊断开关：1=CFPreferences 日志+socket 探针全开（key 前缀 sb-）；0=零足迹。验收/排障时打开

#define SDLOG_TAG_PREFIX "sb"
#define SDLOG_SOCKET 1 // 裸 socket GET 到 Mac（监听 python3 -m http.server 8080）
#include "SDLog.h"
#include "SDCommon.h"
#include "SDALSClient.h"

#pragma mark - 状态

static BOOL gEnabled = YES;
static double gUpLux = SDDEF_UP;            // >= upLux 切浅（支持小数）
static double gDownLux = SDDEF_DOWN;        // <= downLux 切深（支持小数）
static int gInterval = SDDEF_INTERVAL;      // 采样间隔，秒
static int gSwitchDelay = SDDEF_DELAY;      // 切换防抖：越限后持续满足该秒数才切（0=立即）
static BOOL gPauseOnManual = SDDEF_PAUSE;
static int gScheduleMode = 0;               // 0=未选(按面板开关走) 1=日落 2=自定义 3=环境光（选项页第三项写入）
static int gAutomaticOn = 0;                // 系统显示与亮度页"自动"开关镜像（Settings 侧写入）
static int gLowPowerPolicy = SDDEF_LP_POLICY;
static int gLowPowerInterval = SDDEF_LP_INTERVAL;

static BOOL gPausedByManual; // 用户手动切过 → 暂停自动
static BOOL gScreenOff;      // 灭屏 → 停传感器
static BOOL gLowPowerMode;   // 系统低电量模式
static BOOL gSwitching;      // 自己发起的切换（区分手动）
static BOOL gInCCManualToggle; // 正在执行控制中心深色按钮切换
static BOOL gCCTargetDark;     // 控制中心目标外观（YES=深色 NO=浅色）
static double gLastCCStyleChangeAt; // 最近一次控制中心切换时间戳（覆盖异步横幅窗口）
static BOOL gLastCCStyleTargetDark;
static double gLastCCBannerAt; // 最近一次深色模式横幅时间戳（避免重复补发）
static int gRunningInterval; // 当前 ALS 实际生效采样间隔（秒），0=未运行

#if SD_DIAG
static long long gLastPingLux = -1;
static int gLuxEventCount;
#endif
// 防抖挂起状态：越限方向 + 首次满足时间（单调时钟）；回到滞回区或方向反转即清
static long long gPendingMode; // 0=无 1=浅 2=深
static double gPendingSince;

static void ALSEventCallback(void *target, void *refcon, void *queue, void *event);

static inline double MonotonicNow(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static inline int ClampInt(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline double ClampDouble(double v, double lo, double hi)
{
    if (!(v == v)) return lo; // NaN 防护
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline BOOL LowPowerPaused(void)
{
    return gLowPowerMode && gLowPowerPolicy == SDLP_PAUSE;
}

static inline int EffectiveIntervalSec(void)
{
    if (gLowPowerMode && gLowPowerPolicy == SDLP_SLOW) {
        return gLowPowerInterval > gInterval ? gLowPowerInterval : gInterval;
    }
    return gInterval;
}

static void SensorStop(void)
{
    SDALSStop();
    gRunningInterval = 0;
}

// 按有效采样间隔启动传感器；失败（无服务/符号缺）静默返回，下次事件再重启
static void SensorStart(void)
{
    int iv = EffectiveIntervalSec();
    if (SDALSStart((uint32_t)iv * 1000000u, ALSEventCallback)) {
        gRunningInterval = iv;
    }
}

static void SensorRestart(void)
{
    SensorStop();
    SensorStart();
}

static void ApplyPrefsDict(NSDictionary *p)
{
    gEnabled = [p[@SDKEY_ENABLED] ?: @SDDEF_ENABLED boolValue];
    gUpLux = ClampDouble([p[@SDKEY_UP] ?: @SDDEF_UP doubleValue], SDMIN_UP, SDMAX_UP);
    gDownLux = ClampDouble([p[@SDKEY_DOWN] ?: @SDDEF_DOWN doubleValue], SDMIN_DOWN, SDMAX_DOWN);
    gInterval = ClampInt([p[@SDKEY_INTERVAL] ?: @SDDEF_INTERVAL intValue], SDMIN_INTERVAL, SDMAX_INTERVAL);
    gSwitchDelay = ClampInt([p[@SDKEY_DELAY] ?: @SDDEF_DELAY intValue], SDMIN_DELAY, SDMAX_DELAY);
    gPauseOnManual = [p[@SDKEY_PAUSE] ?: @SDDEF_PAUSE boolValue];
    gScheduleMode = [p[@SDKEY_SCHEDMODE] ?: @0 intValue];
    gAutomaticOn = [p[@SDKEY_AUTOON] ?: @0 intValue];
    gLowPowerPolicy = ClampInt([p[@SDKEY_LP_POLICY] ?: @SDDEF_LP_POLICY intValue], SDLP_KEEP, SDLP_PAUSE);
    gLowPowerInterval = ClampInt([p[@SDKEY_LP_INTERVAL] ?: @SDDEF_LP_INTERVAL intValue], SDMIN_LP_INTERVAL, SDMAX_LP_INTERVAL);

    // 逻辑层硬约束：严禁 downLux >= upLux，保底留出最小滞回差 SDMIN_LUX_GAP
    if (gDownLux >= gUpLux - SDMIN_LUX_GAP + 1e-6) {
        gDownLux = gUpLux - SDMIN_LUX_GAP;
        if (gDownLux < SDMIN_DOWN) gDownLux = SDMIN_DOWN;
    }
    gPendingMode = 0;
}

// ctor 阶段专用：直读 plist 文件抢一个早期初值（禁止同步 cfprefsd XPC，防 SB 启动卡死）。
// roothide 下此读取可能失败静默回退默认值——%ctor 末尾的 ReloadPrefsLiveAsync(YES)
// 会用 cfprefsd 真值覆盖纠正（0.1.0-29 根因修复），此处只求"启动头几秒别完全瞎"。
static void LoadPrefs(void)
{
    @autoreleasepool {
        NSDictionary *p = [NSDictionary dictionaryWithContentsOfFile:@SD_PREFS_PATH] ?: @{};
        ApplyPrefsDict(p);
    }
}

// 运行期跨进程同步专用：从 cfprefsd 内存直读，解决 CFPreferences 异步落盘导致读旧值竞态
static NSDictionary *CopyLivePrefsDict(void)
{
    CFStringRef domain = CFSTR(SD_PREFS_DOMAIN);
    CFPreferencesAppSynchronize(domain);
    CFDictionaryRef cfDict = CFPreferencesCopyMultiple(NULL, domain,
                                                       kCFPreferencesCurrentUser,
                                                       kCFPreferencesAnyHost);
    if (cfDict) {
        NSDictionary *d = CFBridgingRelease(cfDict);
        if (d.count > 0) return d;
    }
    return [NSDictionary dictionaryWithContentsOfFile:@SD_PREFS_PATH] ?: @{};
}

// 环境光调度是否在场：面板开关=总闸。mode 3=接管；mode 1/2=系统调度，退场；
// mode 0（没进过选项页）= 旧行为，但系统"自动"开着（automaticOn=1）时必须退场——
// 否则我们的环境光切换会被系统当"手动改外观"，自动调度被秒关（实测：开关拨 4 次全被弹回）
static BOOL AmbientActive(void)
{
    if (!gEnabled) return NO;
    if (gScheduleMode == 3) return YES;
    if (gScheduleMode == 1 || gScheduleMode == 2) return NO;
    return gAutomaticOn == 0;
}

static void PublishRuntimeState(void)
{
    static int tok = -1;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        notify_register_check(SD_NOTE_STATE, &tok);
    });
    if (tok < 0) return;
    uint64_t bits = 0;
    if (gPausedByManual) bits |= SDSTATE_PAUSED_MANUAL;
    if (LowPowerPaused()) bits |= SDSTATE_PAUSED_LP;
    else if (gLowPowerMode && gLowPowerPolicy == SDLP_SLOW) bits |= SDSTATE_SLOW_LP;
    notify_set_state(tok, bits);
}

// 传感器在线状态收敛到唯一不变式：
//   - 未启用/切离模式 → 彻底销毁 HID 客户端（零常驻客户端内存）
//   - 灭屏或低电量暂停 → 停止采样（保留客户端供解锁/退出低电量快速恢复）
//   - 运行中且有效采样间隔变化 → 重启应用新间隔
static void ReconcileSensor(void)
{
    PublishRuntimeState();
    if (!AmbientActive()) {
        gRunningInterval = 0;
        SDALSDestroy();
        return;
    }
    if (gScreenOff || LowPowerPaused()) {
        gPendingMode = 0;
        if (SDALSRunning) SensorStop();
        return;
    }
    int wantInterval = EffectiveIntervalSec();
    if (!SDALSRunning) {
        SensorStart();
    } else if (gRunningInterval != wantInterval) {
        SensorRestart();
    }
}

#pragma mark - 切换

typedef struct {
    long long style;  // 0=none, 1=light, 2=dark
    long long timing; // 0=none, 1=until next transition
} SDStyleOverride;

static IMP o_setOverride;

static BOOL SupportsStyleOverride(Class c)
{
    static int cached = -1;
    if (cached != -1) return cached == 1;
    if (!c) return NO;
    Method m = class_getInstanceMethod(c, @selector(setOverride:));
    if (!m) { cached = 0; return NO; }
    char argBuf[64] = {0};
    method_getArgumentType(m, 2, argBuf, sizeof(argBuf));
    // 兼容 {?=qq} / {UISUserInterfaceStyleModeOverride=qq}
    const char *eq = strchr(argBuf, '=');
    BOOL ok = (argBuf[0] == '{' && eq && strncmp(eq + 1, "qq}", 3) == 0);
    PingFmt("sd-ov-enc?ok=%d&t=%s", ok, argBuf);
    cached = ok ? 1 : 0;
    return ok;
}

static BOOL IsDarkNow(void)
{
    return [UITraitCollection currentTraitCollection].userInterfaceStyle == UIUserInterfaceStyleDark;
}

static BOOL SetStyle(long long mode) // 1=浅 2=深；返回是否成功发起
{
    @autoreleasepool {
        Class c = objc_getClass("UISUserInterfaceStyleMode");
        if (!c) { PingFmt("sd-nostyleclass"); return NO; }
        id m = [[c alloc] init];
        if (!m) return NO;

        long long curMode = 0;
        if ([m respondsToSelector:@selector(modeValue)]) {
            curMode = ((long long (*)(id, SEL))objc_msgSend)(m, @selector(modeValue));
        }

        // 原生“自动”模式（modeValue 100=日落到日出, 102=自定义时段）下，一律用 setOverride: 切换深浅色，
        // 保持 modeValue 不变——这样“自动”开关不会被系统弹回关闭，“选项”行不会消失！
        if (curMode >= 100 && SupportsStyleOverride(c)) {
            SDStyleOverride ov = { .style = mode, .timing = 1 };
            gSwitching = YES;
            ((void (*)(id, SEL, SDStyleOverride))objc_msgSend)(m, @selector(setOverride:), ov);
            gSwitching = NO;
            PingFmt("sd-set-ov?s=%lld&cm=%lld", mode, curMode);
            return YES;
        }

        if (![c instancesRespondToSelector:@selector(setModeValue:)]) {
            PingFmt("sd-nomodevalue"); return NO;
        }
        gSwitching = YES;
        ((void (*)(id, SEL, long long))objc_msgSend)(m, @selector(setModeValue:), mode);
        gSwitching = NO;
        PingFmt("sd-set-mv?s=%lld&cm=%lld", mode, curMode);
        return YES;
    }
}

// 越限 → 挂起计时；持续满足 gSwitchDelay 秒才切；回滞回区/反向越限即清
static void Evaluate(long long lux)
{
    double dlux = (double)lux;
    // 快速路径：落在滞回区间内时零 ObjC 调用直接返回
    if (dlux > gDownLux && dlux < gUpLux) {
        gPendingMode = 0;
        return;
    }

    BOOL dark = IsDarkNow();
    long long want = 0;
    if (dlux >= gUpLux && dark) want = 1;
    else if (dlux <= gDownLux && !dark) want = 2;

    if (want == 0) { gPendingMode = 0; return; }

    double now = MonotonicNow();
    if (gPendingMode != want) {
        gPendingMode = want;
        gPendingSince = now;
        if (gSwitchDelay > 0) return;
    }

    if (gSwitchDelay <= 0 || now - gPendingSince >= gSwitchDelay) {
        PingFmt(want == 1 ? "sd-tolight?lux=%lld" : "sd-todark?lux=%lld", lux);
        gPendingMode = 0;
        SetStyle(want);
    }
}

static void PublishLiveLux(long long lux)
{
    static int tok = -1;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        notify_register_check(SD_NOTE_LUX, &tok);
    });
    if (tok >= 0) {
        // 存 lux + 1，使 0 表示“尚未上报”，>=1 表示真实 lux = state - 1
        uint64_t val = (lux >= 0) ? (uint64_t)(lux + 1) : 1;
        notify_set_state(tok, val);
    }
}

// 事件回调（挂在主 runloop，可直接碰 UIKit）
static void ALSEventCallback(void *target, void *refcon, void *queue, void *event) // NOLINT
{
    long long lux;
    if (!SDALSEventLux(event, &lux)) return;
    PublishLiveLux(lux);

#if SD_DIAG
    // 探针流：变化 >=10、贴近任一阈值（±3）、或每第 30 个事件回报一次（固定 key 复写，防膨胀）。
    // 贴阈值必记：短促越限（如 8→2）变化不足 10 也会触发切换，不记就无法解释"显示 5 却切了"
    gLuxEventCount++;
    if (llabs(lux - gLastPingLux) >= 10 || gLuxEventCount % 30 == 1 ||
        lux <= gDownLux + 3 || lux >= gUpLux - 3) {
        gLastPingLux = lux;
        PingFmt("~lux=%lld", lux); // ~ 前缀=固定 key 复写模式
    }
#endif

    if (!AmbientActive() || gPausedByManual || gScreenOff || LowPowerPaused()) return;
    Evaluate(lux);
}

#pragma mark - 通知

// cfprefsd 后台拉真值 → 主线程重放偏好并收敛传感器状态。
// isStartup=YES（%ctor 末尾）：roothide 下 ctor 直读 plist 可能失败回退默认值，
// 靠这次拉取纠正；且不清手动暂停（开机本就是干净状态）。
static void ReloadPrefsLiveAsync(BOOL isStartup)
{
    dispatch_async(dispatch_get_global_queue(0, 0), ^{
        @autoreleasepool {
            NSDictionary *live = CopyLivePrefsDict();
            dispatch_async(dispatch_get_main_queue(), ^{
                @autoreleasepool {
                    int oldMode = gScheduleMode;
                    ApplyPrefsDict(live);
                    if (!isStartup && gScheduleMode == 3 && oldMode != 3) {
                        gPausedByManual = NO; // 新切到“根据环境光”时清掉手动暂停，立即生效
                    }
                    ReconcileSensor();
                    PingFmt(isStartup
                            ? "sd-prefs-boot?en=%d&m=%d&au=%d&up=%.1f&dn=%.1f&dl=%d&iv=%d&lp=%d&lpi=%d"
                            : "sd-prefs-reloaded?en=%d&m=%d&au=%d&up=%.1f&dn=%.1f&dl=%d&iv=%d&lp=%d&lpi=%d",
                            gEnabled, gScheduleMode, gAutomaticOn, gUpLux, gDownLux, gSwitchDelay, gInterval,
                            gLowPowerPolicy, gLowPowerInterval);
                }
            });
        }
    });
}

static void PrefsChangedNote(CFNotificationCenterRef c, void *o, CFStringRef n,
                             const void *obj, CFDictionaryRef ui)
{
    ReloadPrefsLiveAsync(NO);
}

// 设置页点击「立即恢复」：解除手动暂停并立刻重启采样评估当前环境光
static void ResumeNote(CFNotificationCenterRef c, void *o, CFStringRef n,
                       const void *obj, CFDictionaryRef ui)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!gPausedByManual) return;
        gPausedByManual = NO;
        gPendingMode = 0;
        PublishRuntimeState();
        if (AmbientActive() && !gScreenOff && !LowPowerPaused()) {
            SensorRestart();
        }
        PingFmt("sd-manual-resume-ui");
    });
}

// com.apple.iokit.hid.displayStatus 灭/亮都会发，无 payload；
// 统一按"灭屏"处理停采样，解锁时恢复（LightsOut 同款行为）
static void DisplayNote(CFNotificationCenterRef c, void *o, CFStringRef n,
                        const void *obj, CFDictionaryRef ui)
{
    gScreenOff = YES;
    gPendingMode = 0;
    SensorStop();
    PingFmt("sd-display-note-stop");
}

#pragma mark - hooks

static BOOL IsControlCenterVisible(void)
{
    Class ccCls = objc_getClass("SBControlCenterController");
    if (!ccCls || ![ccCls respondsToSelector:@selector(sharedInstanceIfExists)]) return NO;
    id cc = ((id (*)(Class, SEL))objc_msgSend)(ccCls, @selector(sharedInstanceIfExists));
    if (cc && [cc respondsToSelector:@selector(isVisible)]) {
        return ((BOOL (*)(id, SEL))objc_msgSend)(cc, @selector(isVisible));
    }
    return NO;
}

// 用户的 CC/设置手动切换也会走这里。手动翻转在任何模式（含环境光接管）都暂停
// 自动切换（否则环境光几秒内把用户手动选的深浅色翻回来）；解锁/重选环境光/点「立即恢复」时恢复。
%hook UISUserInterfaceStyleMode
- (void)setModeValue:(long long)value
{
    %orig;
    if (gSwitching) {
        gSwitching = NO;
    } else if ((value == 1 || value == 2) && gPauseOnManual && gEnabled && !gPausedByManual && AmbientActive()) {
        gPausedByManual = YES;
        PublishRuntimeState();
        PingFmt("sd-manual-pause?set=%lld", value);
    }
}
%end

// 原生“自动”模式（modeValue 100/102）下的外观切换走 setOverride:
//   - 我们自己 ALS 发起（gSwitching==YES）：直接放行
//   - 用户通过控制中心切回默认外观（ov.style==0 且处于控制中心操作）：必须放行（否则控制中心只能单向切深色），
//     并按设置暂停自动切换
//   - 后台系统日落/日出定时器清空 override（ov.style==0 且非控制中心操作）：环境光接管时拦截，防与系统调度打架
//   - 用户手动点浅色/深色（ov.style==1/2）：放行并暂停环境光，直到下次解锁或点「立即恢复」
static void H_setOverride(id self, SEL _cmd, SDStyleOverride ov)
{
    if (gSwitching) {
        ((void (*)(id, SEL, SDStyleOverride))o_setOverride)(self, _cmd, ov);
        gSwitching = NO;
        return;
    }
    BOOL fromCC = gInCCManualToggle || IsControlCenterVisible();
    if (gScheduleMode == 3 && gEnabled && ov.style == 0 && !fromCC) {
        PingFmt("sd-supp-clear-ov?t=%lld", ov.timing);
        return;
    }
    ((void (*)(id, SEL, SDStyleOverride))o_setOverride)(self, _cmd, ov);
    if (fromCC) {
        gLastCCStyleChangeAt = MonotonicNow();
        if (gInCCManualToggle) {
            gLastCCStyleTargetDark = gCCTargetDark;
        } else if (ov.style == 2) {
            gLastCCStyleTargetDark = YES;
        } else if (ov.style == 1) {
            gLastCCStyleTargetDark = NO;
        } else {
            // ov.style == 0 为取消覆盖切回相反外观，此时 UIKit trait 尚未异步刷新，取反当前态才是目标态
            gLastCCStyleTargetDark = !IsDarkNow();
        }
    }
    if ((ov.style == 1 || ov.style == 2 || (ov.style == 0 && fromCC)) &&
        gPauseOnManual && gEnabled && !gPausedByManual && AmbientActive()) {
        gPausedByManual = YES;
        PublishRuntimeState();
        PingFmt("sd-manual-pause-ov?s=%lld&t=%lld&cc=%d", ov.style, ov.timing, fromCC);
    }
}

// 控制中心深色模式按钮（Apple 原生 AppearanceModule / ControlCenterUIKit，不依赖 CCSupport，类缺失自动降级）：
//   1. 标记 gInCCManualToggle + 时间窗，确保双向切换（含 setOverride:{0,0}）100% 放行；
//   2. 拦截顶部状态横幅（同步与异步回调均覆盖），用 AppearanceModule.bundle 原生 MANUAL 文案
//      替换「深色模式：7:00前保持打开」/「日出前保持打开」等定时提示。
static IMP o_ccSetSelected, o_ccStatusUpdate;

// 按需缓存 AppearanceModule.bundle 原生短文案，避免每次横幅都加载庞大的 Localizable.loctable
static NSString *gCCDarkManual, *gCCLightManual, *gCCPrefix, *gCCDarkToken, *gCCLightToken;
static BOOL gCCStringsLoaded;

static void InvalidateCCStringsCache(void)
{
    gCCStringsLoaded = NO;
    gCCDarkManual = nil;
    gCCLightManual = nil;
    gCCPrefix = nil;
    gCCDarkToken = nil;
    gCCLightToken = nil;
}

static BOOL EnsureCCAppearanceStrings(void)
{
    if (gCCStringsLoaded) return (gCCDarkManual.length > 0 && gCCLightManual.length > 0);
    gCCStringsLoaded = YES;
    @autoreleasepool {
        Class ccCls = objc_getClass("CCUIAppearanceModule");
        NSBundle *b = ccCls ? [NSBundle bundleForClass:ccCls] : nil;
        if (!b) {
            b = [NSBundle bundleWithPath:@"/System/Library/ControlCenter/Bundles/AppearanceModule.bundle"];
        }
        if (!b) return NO;

        NSString *dm = [b localizedStringForKey:@"CONTROL_CENTER_STATUS_STYLE_MODE_DARK_MANUAL" value:nil table:nil];
        NSString *lm = [b localizedStringForKey:@"CONTROL_CENTER_STATUS_STYLE_MODE_LIGHT_MANUAL" value:nil table:nil];
        if (!dm.length || !lm.length || [dm isEqualToString:@"CONTROL_CENTER_STATUS_STYLE_MODE_DARK_MANUAL"]) {
            return NO;
        }
        gCCDarkManual = [dm copy];
        gCCLightManual = [lm copy];

        NSRange r = [dm rangeOfString:@"："];
        if (r.location == NSNotFound) r = [dm rangeOfString:@":"];
        if (r.location != NSNotFound) {
            gCCPrefix = [[dm substringToIndex:r.location + r.length] copy];
            NSCharacterSet *ws = [NSCharacterSet whitespaceCharacterSet];
            gCCDarkToken = [[[dm substringFromIndex:gCCPrefix.length] stringByTrimmingCharactersInSet:ws] copy];
            if (lm.length >= gCCPrefix.length) {
                gCCLightToken = [[[lm substringFromIndex:gCCPrefix.length] stringByTrimmingCharactersInSet:ws] copy];
            }
        }
    }
    return YES;
}

static void H_ccSetSelected(id self, SEL _cmd, BOOL selected)
{
    double toggleAt = MonotonicNow();
    gInCCManualToggle = YES;
    gCCTargetDark = selected;
    gLastCCStyleChangeAt = toggleAt;
    gLastCCStyleTargetDark = selected;
    ((void (*)(id, SEL, BOOL))o_ccSetSelected)(self, _cmd, selected);
    gInCCManualToggle = NO;

    if (!AmbientActive()) return;

    // 原生“自动”模式下取消 override（例如白天关闭深色模式）时，系统可能不发关闭横幅；
    // 短延迟确认若系统未弹横幅，则用原生 AppearanceModule 文案补发一次。
    __weak id weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(80 * NSEC_PER_MSEC)),
                   dispatch_get_main_queue(), ^{
        if (!AmbientActive() || gLastCCBannerAt >= toggleAt || gLastCCStyleChangeAt > toggleAt + 1e-6) return;
        id strongSelf = weakSelf;
        if (!strongSelf || ![strongSelf respondsToSelector:@selector(contentModuleContext)]) return;
        id ctx = ((id (*)(id, SEL))objc_msgSend)(strongSelf, @selector(contentModuleContext));
        if (!ctx || ![ctx respondsToSelector:@selector(enqueueStatusUpdate:)]) return;
        if (!EnsureCCAppearanceStrings()) return;
        NSString *text = selected ? gCCDarkManual : gCCLightManual;
        Class suCls = objc_getClass("CCUIStatusUpdate");
        if (!suCls || ![suCls respondsToSelector:@selector(statusUpdateWithMessage:type:)]) return;
        id su = ((id (*)(Class, SEL, id, unsigned long long))objc_msgSend)(
            suCls, @selector(statusUpdateWithMessage:type:), text, 0ULL);
        if (su) {
            ((void (*)(id, SEL, id))objc_msgSend)(ctx, @selector(enqueueStatusUpdate:), su);
        }
    });
}

static NSString *SanitizeCCAppearanceBanner(NSString *msg)
{
    if (!msg.length || !AmbientActive() || !EnsureCCAppearanceStrings()) return msg;

    // 已经是纯净的手动文案则直接放行并记录已弹横幅
    if ([msg isEqualToString:gCCDarkManual] || [msg isEqualToString:gCCLightManual]) {
        gLastCCBannerAt = MonotonicNow();
        return msg;
    }

    // 只替换深色模式横幅（前缀如 "深色模式：" / "Dark Mode:" / "Темный режим:"），绝不误伤夜览等其他模块
    if (!gCCPrefix.length || ![msg hasPrefix:gCCPrefix]) return msg;

    gLastCCBannerAt = MonotonicNow();

    // 判定目标是深色还是浅色：同步切换用 gCCTargetDark；否则优先按文案中的开/关关键字判定，再兜底最近一次控制中心目标态
    BOOL wantDark;
    if (gInCCManualToggle) {
        wantDark = gCCTargetDark;
    } else {
        BOOL hasDark = (gCCDarkToken.length > 0 && [msg rangeOfString:gCCDarkToken].location != NSNotFound);
        BOOL hasLight = (gCCLightToken.length > 0 && [msg rangeOfString:gCCLightToken].location != NSNotFound);
        if (hasDark && !hasLight) {
            wantDark = YES;
        } else if (hasLight && !hasDark) {
            wantDark = NO;
        } else if (MonotonicNow() - gLastCCStyleChangeAt < 2.0) {
            wantDark = gLastCCStyleTargetDark;
        } else {
            wantDark = IsDarkNow();
        }
    }
    return wantDark ? gCCDarkManual : gCCLightManual;
}

static id H_ccStatusUpdate(id self, SEL _cmd, NSString *msg, unsigned long long type)
{
    msg = SanitizeCCAppearanceBanner(msg);
    return ((id (*)(id, SEL, id, unsigned long long))o_ccStatusUpdate)(self, _cmd, msg, type);
}

static IMP o_ccStatusMessage;
static id H_ccStatusMessage(id self, SEL _cmd)
{
    NSString *msg = ((id (*)(id, SEL))o_ccStatusMessage)(self, _cmd);
    return SanitizeCCAppearanceBanner(msg);
}

static BOOL HookControlCenterAppearance(void)
{
    if (!o_ccSetSelected) {
        Class ccCls = objc_getClass("CCUIAppearanceModule");
        if (ccCls && class_getInstanceMethod(ccCls, @selector(setSelected:))) {
            MSHookMessageEx(ccCls, @selector(setSelected:), (IMP)H_ccSetSelected, &o_ccSetSelected);
        }
    }
    if (!o_ccStatusUpdate && !o_ccStatusMessage) {
        Class suCls = objc_getClass("CCUIStatusUpdate");
        Class meta = suCls ? object_getClass(suCls) : Nil;
        if (meta && class_getInstanceMethod(meta, @selector(statusUpdateWithMessage:type:))) {
            MSHookMessageEx(meta, @selector(statusUpdateWithMessage:type:), (IMP)H_ccStatusUpdate, &o_ccStatusUpdate);
        } else if (suCls && class_getInstanceMethod(suCls, @selector(message))) {
            MSHookMessageEx(suCls, @selector(message), (IMP)H_ccStatusMessage, &o_ccStatusMessage);
        }
    }
    return (o_ccSetSelected != NULL && (o_ccStatusUpdate != NULL || o_ccStatusMessage != NULL));
}

// 压制系统调度器：SB 内的 DBSDeviceAppearanceScheduleController 若经 set_styleMode: 下发切换，
// 环境光接管时跳过（探针记录它是否真的被调）
static IMP o_dbsSetStyle;
static void H_DBSSetStyle(id self, SEL _cmd, id mode)
{
    if (gScheduleMode == 3 && gEnabled) {
        PingFmt("sd-supp-style?cls=%s", mode ? object_getClassName(mode) : "nil");
        return;
    }
    ((void (*)(id, SEL, id))o_dbsSetStyle)(self, _cmd, mode);
}

// 解锁：清手动暂停 + 恢复采样（iOS17 若改名，表现为灭屏后不恢复，探针可见再修）
%hook CSCoverSheetViewController
- (void)setAuthenticated:(BOOL)authenticated
{
    %orig;
    if (authenticated && (gPausedByManual || gScreenOff)) {
        gPausedByManual = NO;
        gScreenOff = NO;
        ReconcileSensor();
        PingFmt("sd-resume");
    }
}
%end

#pragma mark - 启动

%ctor
{
#if SD_DIAG
    PingFmt("sd-loaded?ver=" SD_VERSION);
#endif

    // 按真实运行时 encoding 校验后安全 hook setOverride:
    {
        Class styleCls = objc_getClass("UISUserInterfaceStyleMode");
        if (styleCls && SupportsStyleOverride(styleCls)) {
            MSHookMessageEx(styleCls, @selector(setOverride:), (IMP)H_setOverride, &o_setOverride);
        }
    }

    // 压制系统调度：SB 内 DBS 控制器经 set_styleMode: 下发的切换，环境光接管时拦截
    {
        Class dbs = objc_getClass("DBSDeviceAppearanceScheduleController");
        if (dbs && class_getInstanceMethod(dbs, @selector(set_styleMode:)))
            MSHookMessageEx(dbs, @selector(set_styleMode:), (IMP)H_DBSSetStyle, &o_dbsSetStyle);
    }

    LoadPrefs();
    gLowPowerMode = [NSProcessInfo processInfo].isLowPowerModeEnabled;

    CFNotificationCenterRef dc = CFNotificationCenterGetDarwinNotifyCenter();
    CFNotificationCenterAddObserver(dc, NULL, PrefsChangedNote,
                                     (CFStringRef)@SD_NOTE_PREFS_CHANGED, NULL,
                                     CFNotificationSuspensionBehaviorDeliverImmediately);
    CFNotificationCenterAddObserver(dc, NULL, ResumeNote,
                                     (CFStringRef)@SD_NOTE_RESUME, NULL,
                                     CFNotificationSuspensionBehaviorDeliverImmediately);
    CFNotificationCenterAddObserver(dc, NULL, DisplayNote,
                                     CFSTR("com.apple.iokit.hid.displayStatus"), NULL,
                                     CFNotificationSuspensionBehaviorDeliverImmediately);

    // 控制中心外观模块按需懒加载：仅关注 ControlCenter/Appearance 相关 bundle，挂齐后立刻移除监听
    if (!HookControlCenterAppearance()) {
        __block id ccObs = nil;
        ccObs = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSBundleDidLoadNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification *note) {
            NSBundle *b = [note.object isKindOfClass:[NSBundle class]] ? (NSBundle *)note.object : nil;
            NSString *bp = b.bundlePath;
            if (bp && [bp rangeOfString:@"ControlCenter"].location == NSNotFound &&
                [bp rangeOfString:@"Appearance"].location == NSNotFound) {
                return;
            }
            if (HookControlCenterAppearance() && ccObs) {
                [[NSNotificationCenter defaultCenter] removeObserver:ccObs];
                ccObs = nil;
            }
        }];
    }

    // 监听系统低电量模式变化（queue:nil 避免额外持有 NSOperationQueue，主队列更新状态并收敛传感器）
    [[NSNotificationCenter defaultCenter] addObserverForName:NSProcessInfoPowerStateDidChangeNotification
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(__unused NSNotification *note) {
        BOOL lpm = [NSProcessInfo processInfo].isLowPowerModeEnabled;
        dispatch_async(dispatch_get_main_queue(), ^{
            gLowPowerMode = lpm;
            ReconcileSensor();
            PingFmt("sd-lpm?on=%d&pol=%d&iv=%d", gLowPowerMode, gLowPowerPolicy, EffectiveIntervalSec());
        });
    }];

    // 系统语言切换时清空控制中心文案缓存
    [[NSNotificationCenter defaultCenter] addObserverForName:NSCurrentLocaleDidChangeNotification
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(__unused NSNotification *note) {
        dispatch_async(dispatch_get_main_queue(), ^{ InvalidateCCStringsCache(); });
    }];

    // 类可能不存在（iOS17 改名）：显式传类，缺了只跳过该 hook 不炸 %init
    %init(UISUserInterfaceStyleMode = NSClassFromString(@"UISUserInterfaceStyleMode"),
          CSCoverSheetViewController = NSClassFromString(@"CSCoverSheetViewController"));

    // dpkg 安装不进 trustcache（Sileo 才注册），借 libjailbreak 导出 API 自注册；
    // 目录级注册顺便覆盖本 tweak + prefs 副本，respring 后由 SB 引导注入恢复全机。
    // 路径双适配：roothide 的 SB 视图里就是 /usr/...；rootless 在 /var/jb 符号链接下，
    // 按存在性选择——roothide 设备上 /var/jb 分支永远走不到
    dispatch_async(dispatch_get_global_queue(0, 0), ^{
        void *h = dlopen("/basebin/libjailbreak.dylib", RTLD_NOW);
        if (!h) return;
        int (*add_file)(const char *) = dlsym(h, "jb_trustcache_add_file");
        int (*add_dir)(const char *) = dlsym(h, "jb_trustcache_add_directory");
        if (add_file && add_dir) {
            static const char *rel[] = {
                "/usr/lib/TweakInject/SenseDark.dylib",
                "/usr/lib/TweakInject",
                "/Library/PreferenceBundles",
            };
            char path[256];
            for (size_t i = 0; i < sizeof(rel) / sizeof(rel[0]); i++) {
                if (access(rel[i], F_OK) == 0) {
                    snprintf(path, sizeof(path), "%s", rel[i]);
                } else {
                    snprintf(path, sizeof(path), "/var/jb%s", rel[i]);
                }
                (i == 0 ? add_file : add_dir)(path);
            }
        }
        dlclose(h);
    });

    ReconcileSensor();
    ReloadPrefsLiveAsync(YES);
}

%dtor { SDALSDestroy(); }
