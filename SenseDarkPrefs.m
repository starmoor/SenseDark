#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>

#include "SDLog.h"
#include "SDCommon.h"
#include "SDLoc.h"
#include "SDPSSpec.h"

@interface PSListController : UIViewController {
@protected
    NSArray *_specifiers;
}
- (NSArray *)specifiers;
- (void)reloadSpecifiers;
- (id)rootController;
- (id)parentController;
@end

@interface SenseDarkPrefsListController : PSListController {
    int _renderedLangCode;
}
@end

@implementation SenseDarkPrefsListController

- (void)updateLocalizedTitle
{
    self.title = SDLoc(@"光感深色", @"SenseDark", @"SenseDark");
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    _renderedLangCode = SDResolvedLangCode();
    [self updateLocalizedTitle];
}

- (void)viewWillAppear:(BOOL)animated
{
    [super viewWillAppear:animated];
    SDInvalidateLangCache();
    int curLang = SDResolvedLangCode();
    if (curLang != _renderedLangCode) {
        _renderedLangCode = curLang;
        [self updateLocalizedTitle];
        [self reloadSpecifiers];
    }
}

- (NSArray *)specifiers
{
    if (!_specifiers) {
        Class specCls = objc_getClass("PSSpecifier");
        NSMutableArray *arr = [NSMutableArray array];
        if (specCls) {
            // 1. 使用指引组 + 跳转按钮
            id guideGrp = ((id (*)(Class, SEL, id, id))objc_msgSend)(
                specCls, @selector(groupSpecifierWithID:name:),
                @"SD_GUIDE_GRP", SDLoc(@"使用指引", @"Guide", @"Руководство"));
            SpecSetProperty(guideGrp, SDLoc(
                @"光感深色（SenseDark）已集成至系统原生外观设置中。\n\n请前往「设置 → 显示与亮度」，打开「自动」开关后点进「选项」，选择「根据环境光」，即可在下方直接调整浅色/深色阈值、切换延迟、采样间隔、低电量策略及手动切换暂停等全部参数。",
                @"SenseDark is integrated into the native Appearance settings.\n\nGo to Settings → Display & Brightness, enable Automatic, tap Options, and select Ambient Light to configure light/dark thresholds, switch delay, sampling interval, Low Power Mode policy, and manual override.",
                @"SenseDark интегрирован в системные настройки оформления.\n\nПерейдите в «Настройки» → «Экран и яркость», включите «Автоматически», откройте «Параметры» и выберите «По освещенности», чтобы настроить пороги, задержку, интервал, режим энергосбережения и паузу при ручной смене."),
                @"footerText");
            if (guideGrp) [arr addObject:guideGrp];

            // PSButtonCell = 13
            id openBtn = ((id (*)(Class, SEL, id, id, SEL, SEL, Class, long, BOOL))objc_msgSend)(
                specCls,
                @selector(preferenceSpecifierNamed:target:set:get:detail:cell:edit:),
                SDLoc(@"前往「显示与亮度」", @"Open Display & Brightness", @"Открыть «Экран и яркость»"),
                self, NULL, NULL, Nil, 13, NO);
            if (openBtn) {
                if ([openBtn respondsToSelector:@selector(setButtonAction:)]) {
                    ((void (*)(id, SEL, SEL))objc_msgSend)(openBtn, @selector(setButtonAction:), @selector(openDisplaySettings:));
                } else {
                    SpecSetProperty(openBtn, NSStringFromSelector(@selector(openDisplaySettings:)), @"action");
                }
                [arr addObject:openBtn];
            }

            // 2. 语言与关于
            id moreGrp = ((id (*)(Class, SEL, id, id))objc_msgSend)(
                specCls, @selector(groupSpecifierWithID:name:), @"SD_MORE_GRP", @"");
            if (moreGrp) [arr addObject:moreGrp];

            id langSpec = ((id (*)(Class, SEL, id, id, SEL, SEL, Class, long, BOOL))objc_msgSend)(
                specCls,
                @selector(preferenceSpecifierNamed:target:set:get:detail:cell:edit:),
                SDLoc(@"语言", @"Language", @"Язык"),
                self, NULL, @selector(getLanguageValue:), Nil, kSDTitleValueCell, NO);
            if (langSpec) {
                SpecSetProperty(langSpec, @"SD_PREFS_LANG", @"id");
                [arr addObject:langSpec];
            }

            id aboutSpec = ((id (*)(Class, SEL, id, id, SEL, SEL, Class, long, BOOL))objc_msgSend)(
                specCls,
                @selector(preferenceSpecifierNamed:target:set:get:detail:cell:edit:),
                SDLoc(@"关于", @"About", @"О программе"),
                self, NULL, @selector(getAboutVersionValue:), Nil, kSDTitleValueCell, NO);
            if (aboutSpec) {
                SpecSetProperty(aboutSpec, @"SD_PREFS_ABOUT", @"id");
                [arr addObject:aboutSpec];
            }
        }
        _specifiers = [arr copy];
    }
    return _specifiers;
}

- (id)getLanguageValue:(id)specifier
{
    return SDCurrentLangDisplayName();
}

- (id)getAboutVersionValue:(id)specifier
{
    return @SD_VERSION;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
    IMP superImp = SuperIMPFor([self class], _cmd);
    UITableViewCell *cell = superImp
        ? ((UITableViewCell *(*)(id, SEL, id, id))superImp)(self, _cmd, tableView, indexPath)
        : nil;
    id sp = SpecAtIndexPath(self, indexPath);
    NSString *ident = SpecIdent(sp);
    if (cell && ([ident isEqualToString:@"SD_PREFS_LANG"] || [ident isEqualToString:@"SD_PREFS_ABOUT"])) {
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    }
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
    id sp = SpecAtIndexPath(self, indexPath);
    NSString *ident = SpecIdent(sp);
    if ([ident isEqualToString:@"SD_PREFS_LANG"]) {
        [tableView deselectRowAtIndexPath:indexPath animated:YES];
        UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
        __weak typeof(self) weakSelf = self;
        SDPresentLanguagePicker(self, cell, ^{
            __strong typeof(weakSelf) strongSelf = weakSelf;
            if (!strongSelf) return;
            strongSelf->_renderedLangCode = SDResolvedLangCode();
            [strongSelf updateLocalizedTitle];
            [strongSelf reloadSpecifiers];
        });
        return;
    }
    if ([ident isEqualToString:@"SD_PREFS_ABOUT"]) {
        [tableView deselectRowAtIndexPath:indexPath animated:YES];
        SDPresentAboutAlert(self, @SD_VERSION);
        return;
    }
    IMP superImp = SuperIMPFor([self class], _cmd);
    if (superImp) {
        ((void (*)(id, SEL, id, id))superImp)(self, _cmd, tableView, indexPath);
    }
}

- (UIViewController *)findPrefsListController
{
    Class prefsListCls = objc_getClass("PSUIPrefsListController");
    if (!prefsListCls) return nil;

    for (UIViewController *vc in self.navigationController.viewControllers) {
        if ([vc isKindOfClass:prefsListCls]) return vc;
    }
    for (UIViewController *vc in self.splitViewController.viewControllers) {
        if ([vc isKindOfClass:prefsListCls]) return vc;
        if ([vc isKindOfClass:[UINavigationController class]]) {
            for (UIViewController *child in ((UINavigationController *)vc).viewControllers) {
                if ([child isKindOfClass:prefsListCls]) return child;
            }
        }
    }
    return nil;
}

- (void)openDisplaySettings:(id)specifier
{
    // 1. 进程内原生跳转：直接构造 DBSSettingsController 并切换导航栈至「设置 → 显示与亮度」
    Class dbsCls = objc_getClass("DBSSettingsController");
    if (!dbsCls) {
        NSBundle *b = [NSBundle bundleWithPath:@"/System/Library/PrivateFrameworks/Settings/DisplayAndBrightnessSettings.framework"];
        [b load];
        dbsCls = objc_getClass("DBSSettingsController");
    }

    UINavigationController *nav = self.navigationController;
    if (dbsCls && nav) {
        UIViewController *prefsList = [self findPrefsListController];
        id displaySpec = nil;
        if (prefsList && [prefsList respondsToSelector:@selector(specifierForID:)]) {
            displaySpec = ((id (*)(id, SEL, id))objc_msgSend)(prefsList, @selector(specifierForID:), @"DISPLAY");
        }

        UIViewController *vc = [[dbsCls alloc] init];
        if (vc) {
            id rootCtrl = [self respondsToSelector:@selector(rootController)] ? [self rootController] : nav;
            if ([vc respondsToSelector:@selector(setRootController:)] && rootCtrl) {
                ((void (*)(id, SEL, id))objc_msgSend)(vc, @selector(setRootController:), rootCtrl);
            }
            if ([vc respondsToSelector:@selector(setParentController:)]) {
                ((void (*)(id, SEL, id))objc_msgSend)(vc, @selector(setParentController:), prefsList ?: self);
            }
            if ([vc respondsToSelector:@selector(setSpecifier:)] && displaySpec) {
                ((void (*)(id, SEL, id))objc_msgSend)(vc, @selector(setSpecifier:), displaySpec);
            }

            if (prefsList && [nav.viewControllers containsObject:prefsList]) {
                [nav setViewControllers:@[prefsList, vc] animated:YES];
            } else {
                [nav pushViewController:vc animated:YES];
            }
            return;
        }
    }

    // 2. 兜底：通过 LSApplicationWorkspace 打开 prefs:root=DISPLAY
    Class wsCls = objc_getClass("LSApplicationWorkspace");
    if (wsCls && [wsCls respondsToSelector:@selector(defaultWorkspace)]) {
        id ws = ((id (*)(Class, SEL))objc_msgSend)(wsCls, @selector(defaultWorkspace));
        NSURL *url = [NSURL URLWithString:@"prefs:root=DISPLAY"];
        if (ws && url && [ws respondsToSelector:@selector(openSensitiveURL:withOptions:)]) {
            ((BOOL (*)(id, SEL, id, id))objc_msgSend)(ws, @selector(openSensitiveURL:withOptions:), url, nil);
        }
    }
}

@end
