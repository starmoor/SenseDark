#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>

@interface PSListController : UIViewController {
@protected
    NSArray *_specifiers;
}
- (NSArray *)specifiers;
- (NSArray *)loadSpecifiersFromPlistName:(NSString *)name target:(id)target;
- (id)rootController;
- (id)parentController;
@end

@interface SenseDarkPrefsListController : PSListController
@end

@implementation SenseDarkPrefsListController

- (void)viewDidLoad
{
    [super viewDidLoad];
    // 中文名「光感深色」随系统中文显示；其余语言回退品牌名
    //（PreferenceLoader entry 行标签不支持本地化，那边直接写中文名）
    NSString *lang = [NSLocale preferredLanguages].firstObject;
    self.title = [lang hasPrefix:@"zh"] ? @"光感深色" : @"SenseDark";
}

- (NSArray *)specifiers
{
    if (!_specifiers) {
        _specifiers = [self loadSpecifiersFromPlistName:@"SenseDark" target:self];
    }
    return _specifiers;
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
