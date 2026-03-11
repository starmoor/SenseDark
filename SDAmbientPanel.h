// 「根据环境光」选项页内嵌面板：偏好读写（cfprefsd 直写+广播）、中英俄三语、
// 实时 lux 读数（轮询 SB 发布的 notify state）、Specifier 动态构造与展开/收起。
// 被 SenseDarkSettings.x 单一 TU include；要求先 include SDLog.h（Ping/PingFmt）。

#import <UIKit/UIKit.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <notify.h>
#import <objc/runtime.h>
#import <objc/message.h>

#include "SDCommon.h"
#include "SDPSSpec.h"

static NSString *const kOurSpecID = @"SENSEDARK_AMBIENT";
static NSString *const kAmbientPrefix = @"SD_AMB_";
static NSString *const kRadioCheckedKey = @"radioGroupCheckedSpecifier";

#pragma mark - 多语言（中文/英语/俄语，其余回退英语）

static NSString *SDLoc(NSString *zh, NSString *en, NSString *ru)
{
    // 语言判定走 dispatch_once 缓存：本函数在实时 lux 的 1s 轮询路径上（FormatLiveLux）
    static NSString *lang;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ lang = [NSLocale preferredLanguages].firstObject; });
    if ([lang hasPrefix:@"zh"]) return zh;
    if ([lang hasPrefix:@"ru"]) return ru;
    return en;
}

static NSString *SDTitleAmbient(void)
{
    return SDLoc(@"根据环境光", @"Ambient Light", @"По освещенности");
}

#pragma mark - 偏好读写（com.starmoor.sensedark 域）

static id ReadPrefObj(NSString *key)
{
    CFPropertyListRef v = CFPreferencesCopyAppValue((__bridge CFStringRef)key, CFSTR(SD_PREFS_DOMAIN));
    return v ? CFBridgingRelease(v) : nil;
}

static int ReadScheduleMode(void)
{
    id v = ReadPrefObj(@SDKEY_SCHEDMODE);
    return [v respondsToSelector:@selector(intValue)] ? [v intValue] : 0;
}

static void WriteKey(NSString *key, NSNumber *value)
{
    CFPreferencesSetValue((__bridge CFStringRef)key, (__bridge CFPropertyListRef)value,
                          CFSTR(SD_PREFS_DOMAIN),
                          kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    CFPreferencesAppSynchronize(CFSTR(SD_PREFS_DOMAIN));
}

static void PostPrefsChanged(void)
{
    CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),
                                         CFSTR(SD_NOTE_PREFS_CHANGED),
                                         NULL, NULL, TRUE);
}

static void WriteScheduleMode(int mode)
{
    WriteKey(@SDKEY_SCHEDMODE, @(mode));
    PostPrefsChanged();
    PingFmt("st-mode-set?m=%d", mode);
}

static BOOL IsRadioItemID(NSString *ident)
{
    return [ident isEqualToString:@"FROM_SUNSET_TO_SUNRISE"] ||
           [ident isEqualToString:@"SCHEDULE"] ||
           [ident isEqualToString:kOurSpecID];
}

static BOOL IsAmbientConfigID(NSString *ident)
{
    return [ident hasPrefix:kAmbientPrefix];
}

#pragma mark - 实时环境光监测（仅在“选项”页选中“根据环境光”且页面可见时运行，离开即停）
// 显示的就是 SpringBoard 用来做切换决策的同一串数（SB 每个采样事件发布 notify state，
// 此处每秒轮询）——绝不另开本地 HID 订阅：0.5s 本地读数与 SB 采样流不是同一串数，
// 短暂越限本地看不见/决策照样发生，且两进程会打架抢传感器的 ReportInterval。

static long long sLiveLux = -1; // -1=尚未读到
static __weak id sLiveController = nil;
static dispatch_source_t sNotifyPollTimer = nil;
static int sLuxNotifyToken = -1;

static NSString *FormatLiveLux(long long lux)
{
    if (lux < 0) return @"--";
    return [NSString stringWithFormat:@"%lld %@", lux, SDLoc(@"lux", @"lux", @"лк")];
}

static void RefreshLiveLuxCellOnMain(void)
{
    id ctrl = sLiveController;
    if (!ctrl) return;
    id luxSpec = SpecForID(ctrl, @"SD_AMB_LUX");
    if (!luxSpec) return;
    NSIndexPath *ip = IndexPathForSpec(ctrl, luxSpec);
    if (!ip) return;
    UITableView *tv = [ctrl respondsToSelector:@selector(table)]
        ? ((UITableView *(*)(id, SEL))objc_msgSend)(ctrl, @selector(table))
        : nil;
    UITableViewCell *cell = tv ? [tv cellForRowAtIndexPath:ip] : nil;
    if (cell) {
        cell.detailTextLabel.text = FormatLiveLux(sLiveLux);
    }
}

static void OnLiveLuxSample(long long lux)
{
    if (lux < 0) lux = 0;
    if (sLiveLux == lux) return;
    sLiveLux = lux;
    if ([NSThread isMainThread]) {
        RefreshLiveLuxCellOnMain();
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{ RefreshLiveLuxCellOnMain(); });
    }
}

static void PollSpringBoardLuxOnce(void)
{
    if (sLuxNotifyToken < 0) {
        notify_register_check(SD_NOTE_LUX, &sLuxNotifyToken);
    }
    if (sLuxNotifyToken < 0) return;
    uint64_t state = 0;
    if (notify_get_state(sLuxNotifyToken, &state) == NOTIFY_STATUS_OK && state > 0) {
        OnLiveLuxSample((long long)(state - 1));
    }
}

static void StopLiveLuxMonitor(void)
{
    if (sNotifyPollTimer) {
        dispatch_source_cancel(sNotifyPollTimer);
        sNotifyPollTimer = nil;
    }
}

static void StartLiveLuxMonitor(id controller)
{
    sLiveController = controller;
    // 先读取一次 SpringBoard 缓存的最新值，进页面瞬间即可显示数字而非空白
    PollSpringBoardLuxOnce();

    if (!sNotifyPollTimer) {
        sNotifyPollTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(sNotifyPollTimer, DISPATCH_TIME_NOW,
                                  (uint64_t)(1 * NSEC_PER_SEC), (uint64_t)(0.2 * NSEC_PER_SEC));
        dispatch_source_set_event_handler(sNotifyPollTimer, ^{
            PollSpringBoardLuxOnce();
        });
        dispatch_resume(sNotifyPollTimer);
    }
}

#pragma mark - 环境光设置项 Target（读写带钳制，滑块值对齐整数）

@interface SDAmbientPrefsTarget : NSObject
+ (instancetype)sharedTarget;
- (id)getLiveLuxForSpecifier:(id)specifier;
- (id)getAmbientPrefForSpecifier:(id)specifier;
- (void)setAmbientPref:(id)value specifier:(id)specifier;
@end

@implementation SDAmbientPrefsTarget

+ (instancetype)sharedTarget
{
    static SDAmbientPrefsTarget *inst;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ inst = [SDAmbientPrefsTarget new]; });
    return inst;
}

- (id)getLiveLuxForSpecifier:(id)specifier
{
    return FormatLiveLux(sLiveLux);
}

- (id)getAmbientPrefForSpecifier:(id)specifier
{
    NSString *key = SpecProp(specifier, @"key");
    id defVal = SpecProp(specifier, @"default");
    id raw = key ? ReadPrefObj(key) : nil;
    id val = raw ?: defVal;
    if (!val) return @0;

    // 滑块值自动对齐整数并限制在 [min, max] 范围内，避免历史大值越界或显示多位小数
    if (SpecCellType(specifier) == kSDSliderCell) {
        double v = round([val doubleValue]);
        NSNumber *minN = SpecProp(specifier, @"min");
        NSNumber *maxN = SpecProp(specifier, @"max");
        if (minN && v < [minN doubleValue]) v = [minN doubleValue];
        if (maxN && v > [maxN doubleValue]) v = [maxN doubleValue];
        return @((int)v);
    }
    return @([val boolValue]);
}

- (void)setAmbientPref:(id)value specifier:(id)specifier
{
    NSString *key = SpecProp(specifier, @"key");
    if (!key) return;

    NSNumber *toWrite;
    if (SpecCellType(specifier) == kSDSliderCell) {
        int iv = (int)lround([value doubleValue]);
        NSNumber *minN = SpecProp(specifier, @"min");
        NSNumber *maxN = SpecProp(specifier, @"max");
        if (minN && iv < [minN intValue]) iv = [minN intValue];
        if (maxN && iv > [maxN intValue]) iv = [maxN intValue];
        toWrite = @(iv);
    } else {
        toWrite = @([value boolValue]);
    }

    WriteKey(key, toWrite);
    PostPrefsChanged();
    PingFmt("st-pref-set?k=%s&v=%d", key.UTF8String, [toWrite intValue]);
}

@end

#pragma mark - Specifier 构造（在选中“根据环境光”时动态展开）

static id MakeGroupSpecifier(NSString *ident, NSString *title, NSString *footer)
{
    Class specCls = objc_getClass("PSSpecifier");
    if (!specCls) return nil;
    id g = nil;
    if ([specCls respondsToSelector:@selector(groupSpecifierWithID:name:)]) {
        g = ((id (*)(Class, SEL, id, id))objc_msgSend)(
            specCls, @selector(groupSpecifierWithID:name:), ident, title);
    }
    if (!g) {
        g = ((id (*)(Class, SEL, id, id, SEL, SEL, Class, long, BOOL))objc_msgSend)(
            specCls,
            @selector(preferenceSpecifierNamed:target:set:get:detail:cell:edit:),
            title, nil, NULL, NULL, Nil, kSDGroupCell, NO);
        SpecSetProperty(g, ident, @"id");
    }
    if (footer.length) {
        SpecSetProperty(g, footer, @"footerText");
    }
    return g;
}

static id MakeSliderSpecifier(NSString *ident, NSString *key, double minVal, double maxVal, int defVal)
{
    Class specCls = objc_getClass("PSSpecifier");
    if (!specCls) return nil;
    id target = [SDAmbientPrefsTarget sharedTarget];
    id sp = ((id (*)(Class, SEL, id, id, SEL, SEL, Class, long, BOOL))objc_msgSend)(
        specCls,
        @selector(preferenceSpecifierNamed:target:set:get:detail:cell:edit:),
        @"",
        target,
        @selector(setAmbientPref:specifier:),
        @selector(getAmbientPrefForSpecifier:),
        Nil,
        kSDSliderCell,
        NO);
    if (!sp) return nil;
    SpecSetProperty(sp, ident, @"id");
    SpecSetProperty(sp, key, @"key");
    SpecSetProperty(sp, @(SD_PREFS_DOMAIN), @"defaults");
    SpecSetProperty(sp, @(minVal), @"min");
    SpecSetProperty(sp, @(maxVal), @"max");
    SpecSetProperty(sp, @(defVal), @"default");
    SpecSetProperty(sp, @YES, @"showValue");
    return sp;
}

static id MakeSwitchSpecifier(NSString *ident, NSString *title, NSString *key, BOOL defVal)
{
    Class specCls = objc_getClass("PSSpecifier");
    if (!specCls) return nil;
    id target = [SDAmbientPrefsTarget sharedTarget];
    id sp = ((id (*)(Class, SEL, id, id, SEL, SEL, Class, long, BOOL))objc_msgSend)(
        specCls,
        @selector(preferenceSpecifierNamed:target:set:get:detail:cell:edit:),
        title,
        target,
        @selector(setAmbientPref:specifier:),
        @selector(getAmbientPrefForSpecifier:),
        Nil,
        kSDSwitchCell,
        NO);
    if (!sp) return nil;
    SpecSetProperty(sp, ident, @"id");
    SpecSetProperty(sp, key, @"key");
    SpecSetProperty(sp, @(SD_PREFS_DOMAIN), @"defaults");
    SpecSetProperty(sp, @(defVal), @"default");
    return sp;
}

static id MakeLiveLuxSpecifier(void)
{
    Class specCls = objc_getClass("PSSpecifier");
    if (!specCls) return nil;
    id target = [SDAmbientPrefsTarget sharedTarget];
    id sp = ((id (*)(Class, SEL, id, id, SEL, SEL, Class, long, BOOL))objc_msgSend)(
        specCls,
        @selector(preferenceSpecifierNamed:target:set:get:detail:cell:edit:),
        SDLoc(@"当前环境光", @"Current Ambient Light", @"Текущая освещенность"),
        target,
        NULL,
        @selector(getLiveLuxForSpecifier:),
        Nil,
        kSDTitleValueCell,
        NO);
    if (!sp) return nil;
    SpecSetProperty(sp, @"SD_AMB_LUX", @"id");
    return sp;
}

static const void *kAmbientSpecsAssocKey = &kAmbientSpecsAssocKey;

// 顶部增加实时光照行，每个滑块单独成组：标题明确标出选项名，组脚清晰解释作用；
// 亮度阈值聚焦低照度区间（0–100 lux），方便精细微调。
static NSArray *AmbientSpecifiersForController(id controller)
{
    NSArray *cached = objc_getAssociatedObject(controller, kAmbientSpecsAssocKey);
    if (cached) return cached;

    NSMutableArray *arr = [NSMutableArray array];
    void (^add)(id) = ^(id sp) { if (sp) [arr addObject:sp]; };

    add(MakeGroupSpecifier(@"SD_AMB_GRP_LUX", @"", nil));
    add(MakeLiveLuxSpecifier());

    add(MakeGroupSpecifier(
        @"SD_AMB_GRP_UP",
        SDLoc(@"浅色阈值（lux）", @"Light Threshold (lux)", @"Порог светлого (лк)"),
        SDLoc(@"环境光达到或高于此亮度时切换为浅色外观。应高于深色阈值，两者之间的差值为防闪烁缓冲。",
              @"Switches to Light appearance when ambient light reaches or exceeds this level. Should be higher than the Dark threshold to prevent flickering.",
              @"Переключает на светлое оформление, когда освещенность достигает или превышает это значение. Должно быть выше порога темного, чтобы избежать мерцания.")));
    add(MakeSliderSpecifier(@"SD_AMB_UP", @SDKEY_UP, 1, 100, SDDEF_UP));

    add(MakeGroupSpecifier(
        @"SD_AMB_GRP_DOWN",
        SDLoc(@"深色阈值（lux）", @"Dark Threshold (lux)", @"Порог темного (лк)"),
        SDLoc(@"环境光低于或等于此亮度时切换为深色外观。数值越小越暗才切换（暗室约 0–10，室内灯光约 15–50）。",
              @"Switches to Dark appearance when ambient light falls to or below this level. Lower values require darker environments (dark room ~0–10, indoor lighting ~15–50).",
              @"Переключает на темное оформление, когда освещенность падает до этого значения или ниже. Чем меньше значение, тем темнее должно быть (темная комната ~0–10, в помещении ~15–50).")));
    add(MakeSliderSpecifier(@"SD_AMB_DOWN", @SDKEY_DOWN, 0, 100, SDDEF_DOWN));

    add(MakeGroupSpecifier(
        @"SD_AMB_GRP_DELAY",
        SDLoc(@"切换延迟（秒）", @"Switch Delay (sec)", @"Задержка переключения (сек)"),
        SDLoc(@"光线越过阈值后需持续保持该秒数才执行切换，防止短暂遮挡传感器误触发（0 秒为立即切换）。",
              @"Ambient light must stay past the threshold for this duration before switching, preventing accidental triggers from brief shadows (0 = instant).",
              @"Освещенность должна оставаться за порогом в течение этого времени перед переключением, чтобы избежать ложных срабатываний (0 = мгновенно).")));
    add(MakeSliderSpecifier(@"SD_AMB_DELAY", @SDKEY_DELAY, 0, 15, SDDEF_DELAY));

    add(MakeGroupSpecifier(
        @"SD_AMB_GRP_INTERVAL",
        SDLoc(@"传感器采样间隔（秒）", @"Sampling Interval (sec)", @"Интервал опроса датчика (сек)"),
        SDLoc(@"环境光传感器的检测周期。数值越小响应越快，数值越大越省电。",
              @"How often the ambient light sensor is polled. Shorter intervals respond faster; longer intervals save battery.",
              @"Частота опроса датчика освещенности. Меньший интервал ускоряет отклик, больший экономит заряд.")));
    add(MakeSliderSpecifier(@"SD_AMB_INTERVAL", @SDKEY_INTERVAL, 1, 15, SDDEF_INTERVAL));

    add(MakeGroupSpecifier(
        @"SD_AMB_GRP_BEHAVIOR",
        SDLoc(@"手动切换", @"Manual Override", @"Ручное переключение"),
        SDLoc(@"在控制中心或设置中手动切换深浅色后，暂时停止环境光自动切换，下次锁屏解锁后自动恢复。",
              @"Pauses automatic ambient switching when you manually change appearance in Control Center or Settings, resuming after the next unlock.",
              @"Приостанавливает автоматическую смену при ручном переключении в Пункте управления или Настройках и возобновляет после разблокировки.")));
    add(MakeSwitchSpecifier(
        @"SD_AMB_PAUSE",
        SDLoc(@"手动切换后暂停自动", @"Pause After Manual Switch", @"Пауза после ручной смены"),
        @SDKEY_PAUSE,
        SDDEF_PAUSE));

    NSArray *out = [arr copy];
    objc_setAssociatedObject(controller, kAmbientSpecsAssocKey, out, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return out;
}

static void SetAmbientSpecifiersVisible(id controller, BOOL show, BOOL animated)
{
    NSArray *specs = AmbientSpecifiersForController(controller);
    if (!specs.count) return;

    BOOL isShown = (IndexOfSpec(controller, specs.firstObject) != NSNotFound);
    if (show) {
        StartLiveLuxMonitor(controller);
    } else {
        StopLiveLuxMonitor();
    }
    if (show == isShown) return;

    if (show) {
        if ([controller respondsToSelector:@selector(insertContiguousSpecifiers:afterSpecifierID:animated:)] &&
            SpecForID(controller, kOurSpecID)) {
            ((void (*)(id, SEL, id, id, BOOL))objc_msgSend)(
                controller, @selector(insertContiguousSpecifiers:afterSpecifierID:animated:),
                specs, kOurSpecID, animated);
        } else if ([controller respondsToSelector:@selector(addSpecifiersFromArray:animated:)]) {
            ((void (*)(id, SEL, id, BOOL))objc_msgSend)(
                controller, @selector(addSpecifiersFromArray:animated:), specs, animated);
        }
        RefreshLiveLuxCellOnMain();
    } else {
        if ([controller respondsToSelector:@selector(removeContiguousSpecifiers:animated:)]) {
            ((void (*)(id, SEL, id, BOOL))objc_msgSend)(
                controller, @selector(removeContiguousSpecifiers:animated:), specs, animated);
        }
    }
}

static void SyncRadioGroupCheckedSpecifier(id controller, id checkedSpec)
{
    id radioGroup = SpecForID(controller, @"SCHEDULE_GROUP");
    if (radioGroup && checkedSpec) {
        SpecSetProperty(radioGroup, checkedSpec, kRadioCheckedKey);
    }
}

static void UpdateVisibleRadioCheckmarks(id controller, UITableView *tv, NSString *checkedID)
{
    if (!tv) return;
    NSArray<NSString *> *ids = @[@"FROM_SUNSET_TO_SUNRISE", @"SCHEDULE", kOurSpecID];
    for (NSString *ident in ids) {
        id sp = SpecForID(controller, ident);
        NSIndexPath *ip = IndexPathForSpec(controller, sp);
        if (!ip) continue;
        UITableViewCell *cell = [tv cellForRowAtIndexPath:ip];
        if (!cell) continue;
        cell.accessoryType = [ident isEqualToString:checkedID]
            ? UITableViewCellAccessoryCheckmark
            : UITableViewCellAccessoryNone;
    }
}
