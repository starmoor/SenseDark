// ALS HID 客户端（SenseDark.dylib / SpringBoard 专用；Settings 侧实时读数走 SB 发布的
// notify state，绝不另开本地订阅——两进程抢 ReportInterval 会互相脱节）
// 符号全部 dlsym（值取自 Apple IOHIDEventTypes.h：type 12=ALS，Level 字段=12<<16），
// 不链私有 tbd；匹配 PrimaryUsagePage 0xff00（Apple vendor-defined）/ PrimaryUsage 4（ALS）。
//
// 生命周期：SDALSStop 只反注册/反调度，client 保留供下次快速重启；Start 幂等。
// 用法：先 #include "SDLog.h"（默认诊断走 PingFmt），可选 #define SDALS_DIAG(...) 覆盖，再 #include。

#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>

#define kSDHIDEventTypeALS 12
#define kSDHIDEventFieldALSLevel (12 << 16)

typedef void (*SDALSEventCallback)(void *target, void *refcon, void *queue, void *event);

#ifndef SDALS_DIAG
#define SDALS_DIAG(...) PingFmt(__VA_ARGS__)
#endif

static void *(*SDALS_ClientCreate)(CFAllocatorRef);
static int (*SDALS_ClientSetMatching)(void *, CFDictionaryRef);
static CFArrayRef (*SDALS_ClientCopyServices)(void *);
static int (*SDALS_ServiceSetProperty)(void *, CFStringRef, CFTypeRef);
static int (*SDALS_ClientSchedule)(void *, CFRunLoopRef, CFStringRef);
static int (*SDALS_ClientRegisterCallback)(void *, SDALSEventCallback, void *, void *);
static int (*SDALS_ClientUnregisterCallback)(void *);
static int (*SDALS_ClientUnschedule)(void *, CFRunLoopRef, CFStringRef);
static uint32_t (*SDALS_EventGetType)(void *);
static long long (*SDALS_EventGetIntegerValue)(void *, int32_t);

static void *SDALSClient;
static BOOL SDALSRunning;
static CFRunLoopRef SDALSRunLoop;

static BOOL SDALSResolveSyms(void)
{
    static BOOL ok = NO;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        SDALS_ClientCreate = dlsym(RTLD_DEFAULT, "IOHIDEventSystemClientCreate");
        SDALS_ClientSetMatching = dlsym(RTLD_DEFAULT, "IOHIDEventSystemClientSetMatching");
        SDALS_ClientCopyServices = dlsym(RTLD_DEFAULT, "IOHIDEventSystemClientCopyServices");
        SDALS_ServiceSetProperty = dlsym(RTLD_DEFAULT, "IOHIDServiceClientSetProperty");
        SDALS_ClientSchedule = dlsym(RTLD_DEFAULT, "IOHIDEventSystemClientScheduleWithRunLoop");
        SDALS_ClientRegisterCallback = dlsym(RTLD_DEFAULT, "IOHIDEventSystemClientRegisterEventCallback");
        SDALS_ClientUnregisterCallback = dlsym(RTLD_DEFAULT, "IOHIDEventSystemClientUnregisterEventCallback");
        SDALS_ClientUnschedule = dlsym(RTLD_DEFAULT, "IOHIDEventSystemClientUnscheduleWithRunLoop");
        SDALS_EventGetType = dlsym(RTLD_DEFAULT, "IOHIDEventGetType");
        SDALS_EventGetIntegerValue = dlsym(RTLD_DEFAULT, "IOHIDEventGetIntegerValue");
        ok = SDALS_ClientCreate && SDALS_ClientSetMatching && SDALS_ClientCopyServices &&
             SDALS_ServiceSetProperty && SDALS_ClientSchedule && SDALS_ClientRegisterCallback &&
             SDALS_EventGetType && SDALS_EventGetIntegerValue;
    });
    if (!ok) SDALS_DIAG("als-syms-missing");
    return ok;
}

// 事件回调里解 lux：非 ALS 事件返回 NO
static BOOL SDALSEventLux(void *event, long long *luxOut)
{
    if (!SDALS_EventGetType || !SDALS_EventGetIntegerValue) return NO;
    if (SDALS_EventGetType(event) != kSDHIDEventTypeALS) return NO;
    *luxOut = SDALS_EventGetIntegerValue(event, kSDHIDEventFieldALSLevel);
    return YES;
}

static void SDALSStop(void)
{
    if (!SDALSClient || !SDALSRunning) return;
    if (SDALS_ClientUnregisterCallback) SDALS_ClientUnregisterCallback(SDALSClient);
    if (SDALS_ClientUnschedule) SDALS_ClientUnschedule(SDALSClient, SDALSRunLoop, kCFRunLoopDefaultMode);
    SDALSRunning = NO;
}

// 切离环境光模式或关闭总开关时调用：释放 IOHIDEventSystemClient，彻底归零 HID 客户端内存
static void SDALSDestroy(void)
{
    SDALSStop();
    if (SDALSClient) {
        CFRelease((CFTypeRef)SDALSClient);
        SDALSClient = NULL;
    }
}

// 建/复用 client → 按 usage 匹配 ALS → 设 ReportInterval(µs) → 挂主 runloop 收事件
static BOOL SDALSStart(uint32_t intervalUs, SDALSEventCallback cb)
{
    if (SDALSRunning) return YES;
    if (!SDALSResolveSyms()) return NO;

    if (!SDALSClient) {
        SDALSClient = SDALS_ClientCreate(kCFAllocatorDefault);
        if (!SDALSClient) { SDALS_DIAG("als-client-create-fail"); return NO; }

        int page = 0xff00, usage = 4;
        CFNumberRef vals[2] = {
            CFNumberCreate(NULL, kCFNumberSInt32Type, &page),
            CFNumberCreate(NULL, kCFNumberSInt32Type, &usage),
        };
        CFStringRef keys[2] = { CFSTR("PrimaryUsagePage"), CFSTR("PrimaryUsage") };
        CFDictionaryRef match = CFDictionaryCreate(NULL, (const void **)keys, (const void **)vals,
                                                   2, &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        SDALS_ClientSetMatching(SDALSClient, match);
        CFRelease(match);
        CFRelease(vals[0]);
        CFRelease(vals[1]);
    }

    CFArrayRef svcs = SDALS_ClientCopyServices(SDALSClient);
    CFIndex n = svcs ? CFArrayGetCount(svcs) : 0;
    SDALS_DIAG("als-start?n=%ld&iv=%u", (long)n, intervalUs);
    if (n <= 0) {
        if (svcs) CFRelease(svcs);
        return NO;
    }

    int ri = (int)intervalUs;
    CFNumberRef interval = CFNumberCreate(NULL, kCFNumberIntType, &ri);
    SDALS_ServiceSetProperty((void *)CFArrayGetValueAtIndex(svcs, 0), CFSTR("ReportInterval"), interval);
    CFRelease(interval);
    CFRelease(svcs);

    SDALSRunLoop = CFRunLoopGetMain();
    SDALS_ClientSchedule(SDALSClient, SDALSRunLoop, kCFRunLoopDefaultMode);
    SDALS_ClientRegisterCallback(SDALSClient, cb, NULL, NULL);
    SDALSRunning = YES;
    return YES;
}
