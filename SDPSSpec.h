// Preferences.framework 私有运行时辅助：PSSpecifier / PSListController 的属性与
// 查询接口全走 objc_msgSend 动态探测（respondsToSelector 先行），类或方法缺失时
// 安全回退不炸——不链私有头文件，按 iOS 15–17 实测签名手写。

#pragma once
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>

// PSCellType 枚举值（Preferences.framework）
enum {
    kSDGroupCell      = 0,
    kSDListItemCell   = 3,
    kSDTitleValueCell = 4,
    kSDSliderCell     = 5,
    kSDSwitchCell     = 6,
};

#pragma mark - PSSpecifier 动态访问

static inline NSString *SpecIdent(id sp)
{
    if (!sp) return nil;
    if ([sp respondsToSelector:@selector(identifier)])
        return ((id (*)(id, SEL))objc_msgSend)(sp, @selector(identifier));
    if ([sp respondsToSelector:@selector(propertyForKey:)])
        return ((id (*)(id, SEL, id))objc_msgSend)(sp, @selector(propertyForKey:), @"id");
    return nil;
}

static inline id SpecProp(id sp, NSString *key)
{
    if (sp && [sp respondsToSelector:@selector(propertyForKey:)])
        return ((id (*)(id, SEL, id))objc_msgSend)(sp, @selector(propertyForKey:), key);
    return nil;
}

static inline long SpecCellType(id sp)
{
    if (sp && [sp respondsToSelector:@selector(cellType)])
        return ((long (*)(id, SEL))objc_msgSend)(sp, @selector(cellType));
    return kSDListItemCell;
}

static inline void SpecSetProperty(id sp, id value, NSString *key)
{
    if (sp && [sp respondsToSelector:@selector(setProperty:forKey:)])
        ((void (*)(id, SEL, id, id))objc_msgSend)(sp, @selector(setProperty:forKey:), value, key);
}

#pragma mark - PSListController 动态查询

static inline id SpecForID(id controller, NSString *ident)
{
    if (controller && [controller respondsToSelector:@selector(specifierForID:)])
        return ((id (*)(id, SEL, id))objc_msgSend)(controller, @selector(specifierForID:), ident);
    return nil;
}

static inline id SpecAtIndexPath(id controller, id indexPath)
{
    if (controller && indexPath && [controller respondsToSelector:@selector(specifierAtIndexPath:)])
        return ((id (*)(id, SEL, id))objc_msgSend)(controller, @selector(specifierAtIndexPath:), indexPath);
    return nil;
}

static inline NSIndexPath *IndexPathForSpec(id controller, id sp)
{
    if (controller && sp && [controller respondsToSelector:@selector(indexPathForSpecifier:)])
        return ((NSIndexPath *(*)(id, SEL, id))objc_msgSend)(controller, @selector(indexPathForSpecifier:), sp);
    return nil;
}

static inline NSInteger IndexOfSpec(id controller, id sp)
{
    if (controller && sp && [controller respondsToSelector:@selector(indexOfSpecifier:)])
        return ((NSInteger (*)(id, SEL, id))objc_msgSend)(controller, @selector(indexOfSpecifier:), sp);
    return NSNotFound;
}

#pragma mark - 方法签名探测

// 沿继承链找 sel 的实现（绕过被 hook 的本类，直达父类——环境光 cell 交给
// PSListController 父类渲染，避开 DBS 子类对时间 picker 的 override）
static inline IMP SuperIMPFor(Class c, SEL sel)
{
    for (Class s = class_getSuperclass(c); s; s = class_getSuperclass(s)) {
        Method m = class_getInstanceMethod(s, sel);
        if (m) return method_getImplementation(m);
    }
    return NULL;
}

// 私有类方法签名在不同 iOS 上 id/BOOL 漂移，按运行时 type encoding 选对应 IMP 形态
static inline BOOL ArgTypeIsObject(Method m, unsigned int idx)
{
    if (!m) return YES;
    char buf[32] = {0};
    method_getArgumentType(m, idx, buf, sizeof(buf));
    return buf[0] == '@';
}

static inline BOOL ReturnTypeIsObject(Method m)
{
    if (!m) return YES;
    char buf[32] = {0};
    method_getReturnType(m, buf, sizeof(buf));
    return buf[0] == '@';
}
