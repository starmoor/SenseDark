// SenseDark（Preferences 侧）：把「根据环境光」第三选项注入原生
// 「设置 → 显示与亮度 → 自动 → 选项」页（DBSDeviceAppearanceScheduleController），
// 选中即展开内嵌配置组（SDAmbientPanel.h）；同时镜像「自动」开关与选项文案，
// 供 SB 侧（Tweak.x）判定调度归属。只注入 Preferences 进程（SenseDarkSettings.plist）。

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <substrate.h>
#include <string.h>

#define SD_DIAG 0 // 诊断开关：1=关键事件写 SD_DEBUG_DOMAIN（st- 前缀）；0=零足迹。验收/排障时打开

#define SDLOG_TAG_PREFIX "st"
#include "SDLog.h"
#include "SDAmbientPanel.h"

#pragma mark - DBSDeviceAppearanceScheduleController hooks（选项页）

static IMP o_loadSpecs, o_schedSpecifiers, o_schedViewWillAppear, o_schedViewWillDisappear, o_didSelect, o_cellForRow, o_heightForRow, o_showScheduleSpecs;

// 必须返回 NSMutableArray（DBSDeviceAppearanceScheduleController 随后会 addObjectsFromArray:）
static id H_loadSpecs(id self, SEL _cmd, id name, id target)
{
    id r = ((id (*)(id, SEL, id, id))o_loadSpecs)(self, _cmd, name, target);
    @try {
        const char *cn = object_getClassName(self);
        if (!cn || !strstr(cn, "DeviceAppearanceSchedule")) return r;
        if (![r isKindOfClass:[NSArray class]] || [(NSArray *)r count] == 0) return r;

        NSMutableArray *m = [r isKindOfClass:[NSMutableArray class]]
            ? (NSMutableArray *)r
            : [(NSArray *)r mutableCopy];

        for (id sp in m) {
            if ([SpecIdent(sp) isEqualToString:kOurSpecID]) return m;
        }

        id template = nil;
        NSUInteger insertAt = NSNotFound;
        for (NSUInteger i = 0; i < m.count; i++) {
            NSString *ident = SpecIdent(m[i]);
            if ([ident isEqualToString:@"SCHEDULE"]) {
                template = m[i];
                insertAt = i + 1;
                break;
            }
        }
        if (!template) {
            for (NSUInteger i = 0; i < m.count; i++) {
                NSString *ident = SpecIdent(m[i]);
                if ([ident isEqualToString:@"FROM_SUNSET_TO_SUNRISE"]) {
                    template = m[i];
                    insertAt = i + 1;
                    break;
                }
            }
        }
        if (!template || insertAt == NSNotFound) return m;

        Class specCls = objc_getClass("PSSpecifier");
        if (!specCls) return m;
        id ours = ((id (*)(Class, SEL, id, id, SEL, SEL, Class, long, BOOL))objc_msgSend)(
            specCls,
            @selector(preferenceSpecifierNamed:target:set:get:detail:cell:edit:),
            SDTitleAmbient(), target ?: self, NULL, NULL, Nil, SpecCellType(template), NO);
        if (!ours) return m;
        SpecSetProperty(ours, kOurSpecID, @"id");

        [m insertObject:ours atIndex:insertAt];
        PingFmt("st-loadspecs-added?at=%lu&n=%lu", (unsigned long)insertAt, (unsigned long)m.count);
        return m;
    } @catch (NSException *e) {
        PingFmt("st-loadspecs-exc?n=%s", e.name.UTF8String ?: "?");
        return r;
    }
}

static void EnsureSystemScheduleCollapsed(id controller)
{
    if (!controller) return;
    if ([controller respondsToSelector:@selector(_showingScheduleSpecifiers)] &&
        [controller respondsToSelector:@selector(showScheduleSpecifiers:animated:)]) {
        BOOL showing = ((BOOL (*)(id, SEL))objc_msgSend)(controller, @selector(_showingScheduleSpecifiers));
        if (showing) {
            ((void (*)(id, SEL, BOOL, BOOL))objc_msgSend)(
                controller, @selector(showScheduleSpecifiers:animated:), NO, NO);
        }
    }
}

static void H_showScheduleSpecs(id self, SEL _cmd, BOOL show, BOOL animated)
{
    if (show && ReadScheduleMode() == 3) {
        show = NO;
    }
    ((void (*)(id, SEL, BOOL, BOOL))o_showScheduleSpecs)(self, _cmd, show, animated);
}

static NSArray *H_schedSpecifiers(id self, SEL _cmd)
{
    NSArray *r = ((NSArray *(*)(id, SEL))o_schedSpecifiers)(self, _cmd);
    if (ReadScheduleMode() == 3) {
        id ours = nil, radioGroup = nil;
        BOOL hasAmbient = NO;
        NSUInteger oursIdx = NSNotFound;
        for (NSUInteger i = 0; i < r.count; i++) {
            id sp = r[i];
            NSString *ident = SpecIdent(sp);
            if ([ident isEqualToString:kOurSpecID]) {
                ours = sp;
                oursIdx = i;
            } else if ([ident isEqualToString:@"SCHEDULE_GROUP"]) {
                radioGroup = sp;
            } else if (IsAmbientConfigID(ident)) {
                hasAmbient = YES;
            }
        }
        if (radioGroup && ours) {
            SpecSetProperty(radioGroup, ours, kRadioCheckedKey);
        }
        // 系统在后台/外观变化后若重建了 _specifiers（reloadSpecifiers），直接把环境光配置组补回，防止退化为自定义时间段
        if (!hasAmbient && oursIdx != NSNotFound && [r isKindOfClass:[NSMutableArray class]]) {
            NSArray *ambient = BuildAmbientSpecifiers();
            if (ambient.count) {
                NSIndexSet *idxs = [NSIndexSet indexSetWithIndexesInRange:NSMakeRange(oursIdx + 1, ambient.count)];
                [(NSMutableArray *)r insertObjects:ambient atIndexes:idxs];
            }
        }
    }
    return r;
}

static void H_schedViewWillAppear(id self, SEL _cmd, BOOL animated)
{
    SDInvalidateLangCache();
    ((void (*)(id, SEL, BOOL))o_schedViewWillAppear)(self, _cmd, animated);

    // 若用户在独立设置页切换了语言再返回此处，同步刷新三语文案
    id ours = SpecForID(self, kOurSpecID);
    if (ours && [ours respondsToSelector:@selector(name)]) {
        NSString *curName = ((id (*)(id, SEL))objc_msgSend)(ours, @selector(name));
        if (![curName isEqualToString:SDTitleAmbient()]) {
            RebuildAmbientPanelForLanguageChange(self);
        }
    }

    if (ReadScheduleMode() == 3) {
        SyncRadioGroupCheckedSpecifier(self, ours);
        EnsureSystemScheduleCollapsed(self);
        SetAmbientSpecifiersVisible(self, YES, NO);
    } else {
        SetAmbientSpecifiersVisible(self, NO, NO);
    }
}

static void H_schedViewWillDisappear(id self, SEL _cmd, BOOL animated)
{
    StopLiveLuxMonitor();
    if (o_schedViewWillDisappear) {
        ((void (*)(id, SEL, BOOL))o_schedViewWillDisappear)(self, _cmd, animated);
    } else {
        static IMP superImp;
        if (!superImp) superImp = SuperIMPFor(objc_getClass("DBSDeviceAppearanceScheduleController"), _cmd);
        if (superImp) ((void (*)(id, SEL, BOOL))superImp)(self, _cmd, animated);
    }
}

static UITableViewCell *H_cellForRow(id self, SEL _cmd, id tv, id ip)
{
    id sp = SpecAtIndexPath(self, ip);
    NSString *ident = SpecIdent(sp);

    // 我们自己的环境光配置 cell 直接交由父类 PSListController 渲染，
    // 绕过 DBSDeviceAppearanceScheduleController 针对时间 picker 的 override，彻底杜绝越界/误转崩溃。
    if (IsAmbientConfigID(ident)) {
        static IMP superImp;
        if (!superImp) superImp = SuperIMPFor(objc_getClass("DBSDeviceAppearanceScheduleController"), _cmd);
        UITableViewCell *cell = superImp
            ? ((UITableViewCell *(*)(id, SEL, id, id))superImp)(self, _cmd, tv, ip)
            : nil;
        ConfigureAmbientCell(cell, sp);
        return cell;
    }

    UITableViewCell *cell;
    if (o_cellForRow) {
        cell = ((UITableViewCell *(*)(id, SEL, id, id))o_cellForRow)(self, _cmd, tv, ip);
    } else {
        static IMP superImp;
        if (!superImp) superImp = SuperIMPFor(objc_getClass("DBSDeviceAppearanceScheduleController"), _cmd);
        cell = superImp
            ? ((UITableViewCell *(*)(id, SEL, id, id))superImp)(self, _cmd, tv, ip)
            : nil;
    }
    if (!cell) return nil;
    CleanupCustomAmbientViews(cell);
    if (!IsRadioItemID(ident)) return cell;

    int mode = ReadScheduleMode();
    if (mode == 3) {
        cell.accessoryType = [ident isEqualToString:kOurSpecID]
            ? UITableViewCellAccessoryCheckmark
            : UITableViewCellAccessoryNone;
    } else if ([ident isEqualToString:kOurSpecID]) {
        cell.accessoryType = UITableViewCellAccessoryNone;
    }
    return cell;
}

static void H_didSelect(id self, SEL _cmd, id tv, id ip)
{
    id sp = SpecAtIndexPath(self, ip);
    NSString *ident = SpecIdent(sp);

    // 我们插入的配置行不透传给系统调度逻辑，自行处理弹窗菜单点击
    if (IsAmbientConfigID(ident)) {
        [(UITableView *)tv deselectRowAtIndexPath:ip animated:YES];
        HandleAmbientRowTap(self, (UITableView *)tv, ip, ident);
        return;
    }

    if ([ident isEqualToString:kOurSpecID]) {
        [(UITableView *)tv deselectRowAtIndexPath:ip animated:YES];

        WriteKey(@SDKEY_ENABLED, @1);
        WriteKey(@SDKEY_AUTOON, @1);
        WriteScheduleMode(3);

        // 收起自定义时间段（若展开），再丝滑展开环境光设置组
        if ([self respondsToSelector:@selector(_showingScheduleSpecifiers)] &&
            [self respondsToSelector:@selector(showScheduleSpecifiers:animated:)]) {
            BOOL showing = ((BOOL (*)(id, SEL))objc_msgSend)(self, @selector(_showingScheduleSpecifiers));
            if (showing) {
                ((void (*)(id, SEL, BOOL, BOOL))objc_msgSend)(
                    self, @selector(showScheduleSpecifiers:animated:), NO, YES);
            }
        }

        SyncRadioGroupCheckedSpecifier(self, sp);
        UpdateVisibleRadioCheckmarks(self, (UITableView *)tv, kOurSpecID);
        SetAmbientSpecifiersVisible(self, YES, YES);
        Ping("st-tap-ours");
        return;
    }

    BOOL isSunset = [ident isEqualToString:@"FROM_SUNSET_TO_SUNRISE"];
    BOOL isCustom = [ident isEqualToString:@"SCHEDULE"];
    BOOL wasAmbient = (ReadScheduleMode() == 3);

    if (isSunset || isCustom) {
        // 先收起环境光设置组，再交回系统处理（含展开自定义时间组）
        SetAmbientSpecifiersVisible(self, NO, YES);
        WriteScheduleMode(isSunset ? 1 : 2);
        if (wasAmbient) {
            SyncRadioGroupCheckedSpecifier(self, sp);
        }
        PingFmt("st-tap-sys?id=%s", isSunset ? "sunset" : "custom");
    }

    ((void (*)(id, SEL, id, id))o_didSelect)(self, _cmd, tv, ip);

    if ((isSunset || isCustom) && wasAmbient) {
        UpdateVisibleRadioCheckmarks(self, (UITableView *)tv, ident);
    }
}

static CGFloat H_heightForRow(id self, SEL _cmd, id tv, id ip)
{
    id sp = SpecAtIndexPath(self, ip);
    if (IsAmbientConfigID(SpecIdent(sp))) {
        CGFloat customH = HeightForAmbientSpecifier(sp);
        if (customH > 0) return customH;
    }
    if (o_heightForRow) {
        return ((CGFloat (*)(id, SEL, id, id))o_heightForRow)(self, _cmd, tv, ip);
    }
    static IMP superImp;
    if (!superImp) superImp = SuperIMPFor(objc_getClass("DBSDeviceAppearanceScheduleController"), _cmd);
    if (superImp) {
        return ((CGFloat (*)(id, SEL, id, id))superImp)(self, _cmd, tv, ip);
    }
    return UITableViewAutomaticDimension;
}

static void HookScheduleController(void)
{
    static int state;
    if (state) return;
    Class c = objc_getClass("DBSDeviceAppearanceScheduleController");
    if (!c) return;

    if (class_getInstanceMethod(c, @selector(specifiers)))
        MSHookMessageEx(c, @selector(specifiers), (IMP)H_schedSpecifiers, &o_schedSpecifiers);
    if (class_getInstanceMethod(c, @selector(viewWillAppear:)))
        MSHookMessageEx(c, @selector(viewWillAppear:), (IMP)H_schedViewWillAppear, &o_schedViewWillAppear);
    if (class_getInstanceMethod(c, @selector(viewWillDisappear:)))
        MSHookMessageEx(c, @selector(viewWillDisappear:), (IMP)H_schedViewWillDisappear, &o_schedViewWillDisappear);
    else
        class_addMethod(c, @selector(viewWillDisappear:), (IMP)H_schedViewWillDisappear, "v@:B");
    if (class_getInstanceMethod(c, @selector(tableView:didSelectRowAtIndexPath:)))
        MSHookMessageEx(c, @selector(tableView:didSelectRowAtIndexPath:), (IMP)H_didSelect, &o_didSelect);

    BOOL hasCell = class_getInstanceMethod(c, @selector(tableView:cellForRowAtIndexPath:)) != NULL;
    if (hasCell)
        MSHookMessageEx(c, @selector(tableView:cellForRowAtIndexPath:), (IMP)H_cellForRow, &o_cellForRow);
    else
        class_addMethod(c, @selector(tableView:cellForRowAtIndexPath:), (IMP)H_cellForRow, "@@:@@");

    if (class_getInstanceMethod(c, @selector(tableView:heightForRowAtIndexPath:)))
        MSHookMessageEx(c, @selector(tableView:heightForRowAtIndexPath:), (IMP)H_heightForRow, &o_heightForRow);
    else
        class_addMethod(c, @selector(tableView:heightForRowAtIndexPath:), (IMP)H_heightForRow, "d@:@@");

    if (class_getInstanceMethod(c, @selector(showScheduleSpecifiers:animated:)))
        MSHookMessageEx(c, @selector(showScheduleSpecifiers:animated:), (IMP)H_showScheduleSpecs, &o_showScheduleSpecs);

    Class psl = objc_getClass("PSListController");
    if (psl && class_getInstanceMethod(psl, @selector(loadSpecifiersFromPlistName:target:)))
        MSHookMessageEx(psl, @selector(loadSpecifiersFromPlistName:target:), (IMP)H_loadSpecs, &o_loadSpecs);

    [[NSNotificationCenter defaultCenter] addObserverForName:UIApplicationWillEnterForegroundNotification
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(__unused NSNotification *note) {
        dispatch_async(dispatch_get_main_queue(), ^{
            id ctrl = sLiveController;
            if (!ctrl || ReadScheduleMode() != 3) return;
            EnsureSystemScheduleCollapsed(ctrl);
            SetAmbientSpecifiersVisible(ctrl, YES, NO);
            PollSpringBoardLuxOnce();
            RefreshLiveStatusCellsOnMain();
        });
    }];

    state = 1;
    PingFmt("st-sched-hooked?cell=%d&ls=%d", hasCell, o_loadSpecs != NULL);
}

#pragma mark - DBSSettingsController hooks（“显示与亮度”主页）

static IMP o_setAuto, o_getAuto, o_schedStr, o_displayVWA;

// “自动”开关镜像：关=连坐停用环境光（enabled=0、mode 归零）；开=只广播让 SB 侧按 mode 定
static void HandleAutoSet(BOOL on, int prevMode)
{
    WriteKey(@SDKEY_AUTOON, @(on ? 1 : 0));
    if (!on) {
        WriteKey(@SDKEY_ENABLED, @0);
        WriteScheduleMode(0);
    } else {
        PostPrefsChanged();
    }
    PingFmt("st-auto-set?on=%d&prev=%d", on, prevMode);
}

// 系统侧（如日出日落自身开关）改变自动状态时，静默镜像进域供 SB 侧 AmbientActive 判定
static void MirrorAutoGet(BOOL on)
{
    static int last = -1;
    if (last != (int)on) {
        last = (int)on;
        WriteKey(@SDKEY_AUTOON, @(on ? 1 : 0));
    }
}

static void H_setAuto_Obj(id self, SEL _cmd, id val, id spec)
{
    BOOL on = [val boolValue];
    int prevMode = ReadScheduleMode();
    HandleAutoSet(on, prevMode);
    ((void (*)(id, SEL, id, id))o_setAuto)(self, _cmd, val, spec);
}

static void H_setAuto_Bool(id self, SEL _cmd, BOOL on, id spec)
{
    int prevMode = ReadScheduleMode();
    HandleAutoSet(on, prevMode);
    ((void (*)(id, SEL, BOOL, id))o_setAuto)(self, _cmd, on, spec);
}

static id H_getAuto_Obj(id self, SEL _cmd, id spec)
{
    id v = ((id (*)(id, SEL, id))o_getAuto)(self, _cmd, spec);
    MirrorAutoGet([v boolValue]);
    return v;
}

static BOOL H_getAuto_Bool(id self, SEL _cmd, id spec)
{
    BOOL v = ((BOOL (*)(id, SEL, id))o_getAuto)(self, _cmd, spec);
    MirrorAutoGet(v);
    return v;
}

// 环境光接管时，“自动”行的副标题（当前选项名）显示为「根据环境光」
static id H_schedStr(id self, SEL _cmd)
{
    if (ReadScheduleMode() == 3) return SDTitleAmbient();
    return ((id (*)(id, SEL))o_schedStr)(self, _cmd);
}

// 进页刷新副标题（系统默认不重算已渲染的值）
static void H_displayVWA(id self, SEL _cmd, BOOL animated)
{
    SDInvalidateLangCache();
    ((void (*)(id, SEL, BOOL))o_displayVWA)(self, _cmd, animated);
    if ([self respondsToSelector:@selector(_deviceAppearanceScheduleSpecifier)] &&
        [self respondsToSelector:@selector(reloadSpecifier:animated:)]) {
        id schedSpec = ((id (*)(id, SEL))objc_msgSend)(self, @selector(_deviceAppearanceScheduleSpecifier));
        if (schedSpec && IndexOfSpec(self, schedSpec) != NSNotFound) {
            ((void (*)(id, SEL, id, BOOL))objc_msgSend)(self, @selector(reloadSpecifier:animated:), schedSpec, NO);
        }
    }
}

static void HookDisplayController(void)
{
    static int state;
    if (state) return;
    Class c = objc_getClass("DBSSettingsController");
    if (!c) return;

    SEL s = @selector(setAutomaticAppearanceEnabled:forSpecifier:);
    SEL g = @selector(getAutomaticAppearanceEnabledForSpecifier:);
    Method ms = class_getInstanceMethod(c, s);
    Method mg = class_getInstanceMethod(c, g);

    BOOL setIsObj = ArgTypeIsObject(ms, 2);
    BOOL getIsObj = ReturnTypeIsObject(mg);

    if (ms) MSHookMessageEx(c, s, setIsObj ? (IMP)H_setAuto_Obj : (IMP)H_setAuto_Bool, &o_setAuto);
    if (mg) MSHookMessageEx(c, g, getIsObj ? (IMP)H_getAuto_Obj : (IMP)H_getAuto_Bool, &o_getAuto);

    SEL ss = @selector(_deviceAppearanceScheduleString);
    if (class_getInstanceMethod(c, ss))
        MSHookMessageEx(c, ss, (IMP)H_schedStr, &o_schedStr);

    SEL vwa = @selector(viewWillAppear:);
    if (class_getInstanceMethod(c, vwa))
        MSHookMessageEx(c, vwa, (IMP)H_displayVWA, &o_displayVWA);

    state = 1;
    PingFmt("st-display-hooked?ss=%d&so=%d&go=%d", o_schedStr != NULL, setIsObj, getIsObj);
}

#if SD_DIAG
// 诊断探针：仅调试模式开启，发布版零 UIViewController 拦截开销
%hook UIViewController
- (void)viewDidAppear:(BOOL)animated
{
    %orig;
    const char *cn = object_getClassName(self);
    if (cn && (strcmp(cn, "DBSSettingsController") == 0 ||
               strcmp(cn, "DBSDeviceAppearanceScheduleController") == 0)) {
        PingFmt("st-vc?c=%s&m=%d", cn, ReadScheduleMode());
    }
}
%end
#endif

%ctor
{
#if SD_DIAG
    Ping("st-loaded?ver=" SD_VERSION);
#endif
    HookScheduleController();
    HookDisplayController();

    // DBS 私有框架按需加载：仅关注 Display/Brightness 相关 bundle，挂齐后立刻移除监听
    if (!o_loadSpecs || !o_schedStr) {
        __block id bundleObs = nil;
        bundleObs = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSBundleDidLoadNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification *note) {
            NSBundle *b = [note.object isKindOfClass:[NSBundle class]] ? (NSBundle *)note.object : nil;
            NSString *bp = b.bundlePath;
            if (bp && [bp rangeOfString:@"Display"].location == NSNotFound &&
                [bp rangeOfString:@"Brightness"].location == NSNotFound) {
                return;
            }
            HookScheduleController();
            HookDisplayController();
            if (o_loadSpecs && o_schedStr && bundleObs) {
                [[NSNotificationCenter defaultCenter] removeObserver:bundleObs];
                bundleObs = nil;
            }
        }];
    }

#if SD_DIAG
    %init;
#endif
}
