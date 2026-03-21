// SenseDark UI 本地化与通用弹窗（仅 Preferences 侧引用，绝不进入 SpringBoard）：
//   - 完整支持 简体中文（zh）/ 英语（en）/ 俄语（ru），默认跟随系统语言
//   - 支持设置内即时切换语言（存入 com.starmoor.sensedark 域 language 键）
//   - 提供统一的语言选择 ActionSheet 与简洁“关于”弹窗（版本、简介、GitHub 链接）

#pragma once

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include "SDCommon.h"

// 0=en, 1=zh, 2=ru；-1=未初始化（读一次后缓存为整数，避免 1s 轮询路径反复读偏好/分配字符串）
static int sSDResolvedLang = -1;

static inline NSString *SDReadLangPref(void)
{
    CFPropertyListRef v = CFPreferencesCopyAppValue(CFSTR(SDKEY_LANG), CFSTR(SD_PREFS_DOMAIN));
    if (!v) return @"system";
    id obj = CFBridgingRelease(v);
    if ([obj isKindOfClass:[NSString class]]) {
        NSString *s = (NSString *)obj;
        if ([s isEqualToString:@"zh"] || [s isEqualToString:@"en"] || [s isEqualToString:@"ru"]) {
            return s;
        }
    }
    return @"system";
}

static inline void SDInvalidateLangCache(void)
{
    sSDResolvedLang = -1;
}

static inline int SDResolvedLangCode(void)
{
    if (sSDResolvedLang >= 0) return sSDResolvedLang;

    NSString *pref = SDReadLangPref();
    if ([pref isEqualToString:@"zh"]) {
        sSDResolvedLang = 1;
    } else if ([pref isEqualToString:@"ru"]) {
        sSDResolvedLang = 2;
    } else if ([pref isEqualToString:@"en"]) {
        sSDResolvedLang = 0;
    } else {
        NSString *sys = [NSLocale preferredLanguages].firstObject;
        if ([sys hasPrefix:@"zh"]) sSDResolvedLang = 1;
        else if ([sys hasPrefix:@"ru"]) sSDResolvedLang = 2;
        else sSDResolvedLang = 0;
    }
    return sSDResolvedLang;
}

static inline void SDWriteLangPref(NSString *code)
{
    NSString *val = ([code isEqualToString:@"zh"] || [code isEqualToString:@"en"] || [code isEqualToString:@"ru"])
        ? code : @"system";
    CFPreferencesSetValue(CFSTR(SDKEY_LANG), (__bridge CFPropertyListRef)val,
                          CFSTR(SD_PREFS_DOMAIN),
                          kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    CFPreferencesAppSynchronize(CFSTR(SD_PREFS_DOMAIN));
    SDInvalidateLangCache();
    // 同步触发 Preferences 进程内另一页面（如独立设置页 <-> 显示与亮度页）刷新语言缓存
    CFNotificationCenterPostNotification(CFNotificationCenterGetLocalCenter(),
                                         CFSTR("com.starmoor.sensedark.langchanged"),
                                         NULL, NULL, TRUE);
}

static inline NSString *SDLoc(NSString *zh, NSString *en, NSString *ru)
{
    int code = SDResolvedLangCode();
    if (code == 1) return zh;
    if (code == 2) return ru;
    return en;
}

static inline NSString *SDCurrentLangDisplayName(void)
{
    NSString *pref = SDReadLangPref();
    if ([pref isEqualToString:@"zh"]) return @"简体中文";
    if ([pref isEqualToString:@"en"]) return @"English";
    if ([pref isEqualToString:@"ru"]) return @"Русский";
    return SDLoc(@"跟随系统", @"Follow System", @"Как в системе");
}

static inline void SDPresentLanguagePicker(UIViewController *vc, UIView *sourceView, void (^onChanged)(void))
{
    if (!vc) return;
    NSString *cur = SDReadLangPref();
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:SDLoc(@"语言", @"Language", @"Язык")
                                                                   message:nil
                                                            preferredStyle:UIAlertControllerStyleActionSheet];

    NSArray<NSArray<NSString *> *> *items = @[
        @[@"system", SDLoc(@"跟随系统", @"Follow System", @"Как в системе")],
        @[@"zh",     @"简体中文"],
        @[@"en",     @"English"],
        @[@"ru",     @"Русский"],
    ];

    for (NSArray<NSString *> *it in items) {
        NSString *code = it[0];
        NSString *title = [cur isEqualToString:code]
            ? [NSString stringWithFormat:@"✓ %@", it[1]]
            : it[1];
        [sheet addAction:[UIAlertAction actionWithTitle:title
                                                  style:UIAlertActionStyleDefault
                                                handler:^(__unused UIAlertAction *action) {
            if (![cur isEqualToString:code]) {
                SDWriteLangPref(code);
                if (onChanged) onChanged();
            }
        }]];
    }

    [sheet addAction:[UIAlertAction actionWithTitle:SDLoc(@"取消", @"Cancel", @"Отмена")
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];

    UIPopoverPresentationController *pop = sheet.popoverPresentationController;
    if (pop) {
        pop.sourceView = sourceView ?: vc.view;
        pop.sourceRect = sourceView ? sourceView.bounds : CGRectMake(CGRectGetMidX(vc.view.bounds), CGRectGetMidY(vc.view.bounds), 1, 1);
    }
    [vc presentViewController:sheet animated:YES completion:nil];
}

static inline void SDPresentAboutAlert(UIViewController *vc, NSString *version)
{
    if (!vc) return;
    NSString *verStr = version.length ? version : @"0.1.0";
    NSString *title = SDLoc(@"关于光感深色", @"About SenseDark", @"О SenseDark");
    NSString *summary = SDLoc(
        @"基于环境光传感器（lux）自动切换系统深浅色外观，内置滞回防抖、低电量策略与锁屏省电机制。",
        @"Automatically switches system Light/Dark appearance using the ambient light sensor (lux), with hysteresis, debounce, and Low Power Mode support.",
        @"Автоматически переключает светлое и темное оформление по датчику освещенности (лк) с гистерезисом, задержкой и поддержкой режима энергосбережения.");
    NSString *msg = [NSString stringWithFormat:@"%@ %@\n\n%@\n\n%s",
                     SDLoc(@"版本", @"Version", @"Версия"),
                     verStr,
                     summary,
                     SD_GITHUB_URL];

    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title
                                                                   message:msg
                                                            preferredStyle:UIAlertControllerStyleAlert];

    [alert addAction:[UIAlertAction actionWithTitle:SDLoc(@"GitHub 开源仓库", @"GitHub Repository", @"Репозиторий GitHub")
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        NSURL *url = [NSURL URLWithString:@SD_GITHUB_URL];
        if (url) {
            [[UIApplication sharedApplication] openURL:url options:@{} completionHandler:nil];
        }
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:SDLoc(@"好", @"OK", @"ОК")
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];

    [vc presentViewController:alert animated:YES completion:nil];
}
