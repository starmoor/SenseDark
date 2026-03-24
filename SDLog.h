// 共享异步诊断日志（SenseDark.dylib 前缀 sb- / SenseDarkSettings.dylib 前缀 st-）
// 通道一：CFPreferences 域 SD_DEBUG_DOMAIN（见 SDCommon.h）——cfprefsd 代写，SB/Settings 沙盒免疫
//         （fopen /var/mobile 会被静默拒）；读法：同名 debug plist
// 通道二（SDLOG_SOCKET=1 时）：裸 socket GET 到 Mac，监听端 python3 -m http.server 8080
// 铁律（0.1.0-20 白屏教训）：绝不在调用线程同步写 cfprefsd——一律后台块内单次同步写；
// ctor 零 XPC。'~' 前缀 key = 固定复写（高频流式值防 debug plist 无限膨胀）。
//
// SD_DIAG=0 时实现整体编译剔除（Ping/PingFmt 宏化为空语句），诊断字符串零残留。
// 用法：先 #define SD_DIAG（0/1）、可选 SDLOG_TAG_PREFIX / SDLOG_SOCKET / SDLOG_SOCKET_HOST，
// 再 #include。与 control 的 Version 保持同步。

#pragma once

#ifndef SD_DIAG
#define SD_DIAG 0
#endif

#define SD_VERSION "0.1.0-40"

#if SD_DIAG

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <Foundation/Foundation.h>

#include "SDCommon.h" // SD_DEBUG_DOMAIN
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef SDLOG_TAG_PREFIX
#define SDLOG_TAG_PREFIX "sd"
#endif

#ifndef SDLOG_SOCKET
#define SDLOG_SOCKET 0
#endif

#ifndef SDLOG_SOCKET_HOST
#define SDLOG_SOCKET_HOST "127.0.0.1" // 调试时命令行 -DSDLOG_SOCKET_HOST=... 覆盖；默认值不带真实内网地址
#endif

static int SDLogSeq;
static char SDLogLaunchTag[16];
static dispatch_once_t SDLogOnceToken;

static void Ping(const char *tag)
{
    dispatch_once(&SDLogOnceToken, ^{
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(SDLogLaunchTag, sizeof(SDLogLaunchTag), "%H%M%S", &tmv);
    });

    char key[340], val[24];
    const char *k;
    if (tag[0] == '~') {
        k = tag;
    } else {
        snprintf(key, sizeof(key), "%s-%s-%04d %s", SDLOG_TAG_PREFIX, SDLogLaunchTag,
                 __sync_fetch_and_add(&SDLogSeq, 1) + 1, tag);
        k = key;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tmv;
    localtime_r(&tv.tv_sec, &tmv);
    strftime(val, sizeof(val), "%H:%M:%S", &tmv);
    NSString *keyStr = @(k), *valStr = @(val);
    dispatch_async(dispatch_get_global_queue(0, 0), ^{
        CFPreferencesSetValue((__bridge CFStringRef)keyStr,
                              (__bridge CFPropertyListRef)valStr,
                              CFSTR(SD_DEBUG_DOMAIN),
                              kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
        CFPreferencesAppSynchronize(CFSTR(SD_DEBUG_DOMAIN));
    });

#if SDLOG_SOCKET
    char req[384];
    snprintf(req, sizeof(req), "GET /%s?pid=%d HTTP/1.0\r\nHost: %s\r\n\r\n",
             tag, getpid(), SDLOG_SOCKET_HOST);
    char *reqCopy = strdup(req); // block 不能捕获栈数组
    dispatch_async(dispatch_get_global_queue(0, 0), ^{
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { free(reqCopy); return; }
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(8080);
        inet_pton(AF_INET, SDLOG_SOCKET_HOST, &addr.sin_addr);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            write(fd, reqCopy, strlen(reqCopy));
        close(fd);
        free(reqCopy);
    });
#endif
}

static void PingFmt(const char *fmt, ...)
{
    char tag[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tag, sizeof(tag), fmt, ap);
    va_end(ap);
    Ping(tag);
}

#else // !SD_DIAG：零足迹空操作

#define Ping(...)    ((void)0)
#define PingFmt(...) ((void)0)

#endif
