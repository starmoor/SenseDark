// SDUpdateCheck：联网检查更新（SB 侧）
//
// 定期拉 apt 源 Packages 里的最新版本号（结果存偏好 uc.latest；双 scheme 同步发版，
// 取全部 sensedark 条目最大版本）；同一时机上报本机版本信息
// （app/ver/os/uid；uid 为本机首启生成的随机值，非硬件 ID）。
// tag = HMAC-SHA256(salt, uid) 前 16 hex，salt 随版本更换（服务端登记同一份）。
//
// 节奏：首查在首启后 30min~3h 随机；此后每次成功联网 24~72h 随机、全程失败 1~3h 重试
// ——无固定周期可观察。状态存 cfpregsd 域 SD_PREFS_DOMAIN（uc.* 键，与用户偏好键不冲突）。
// 铁律：所有 cfpregsd/网络操作只在后台串行队列，%ctor 只挂定时器（零 XPC）。

#import "SDUpdateCheck.h"
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonHMAC.h>
#import "SDCommon.h"
#import "SDLog.h" // SD_VERSION

#define UC_APP         "sensedark"
#define UC_SALT        "1e91d64dabc70658c29079b86fc81701" // 与服务端登记的 0.1.0-41 salt 一致；发版必换并同步登记

static NSString *const UCPackagesURL = @"https://repo.rusti.cc/ios/Packages";
static NSString *const UCPingURL = @"https://t.rusti.cc/v1/ping";

// 秒级随机区间
static int64_t UCRandRange(int64_t lo, int64_t hi)
{
    return lo + (int64_t)arc4random_uniform((uint32_t)(hi - lo + 1));
}

// ---------- cfpregsd 读写（仅在后台队列调用） ----------

static NSDictionary *UCReadState(void)
{
    CFStringRef domain = CFSTR(SD_PREFS_DOMAIN);
    CFDictionaryRef d = CFPreferencesCopyMultiple(NULL, domain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    NSDictionary *raw = d ? CFBridgingRelease(d) : @{};
    NSMutableDictionary *out = [NSMutableDictionary dictionary];
    for (NSString *k in @[ @"uc.uid", @"uc.at", @"uc.latest" ]) {
        id v = raw[k];
        if (v) out[k] = v;
    }
    return out;
}

static void UCWriteState(NSDictionary *kv)
{
    CFStringRef domain = CFSTR(SD_PREFS_DOMAIN);
    for (NSString *k in kv) {
        CFPreferencesSetValue((CFStringRef)k, (__bridge CFPropertyListRef)kv[k],
                              domain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    }
    CFPreferencesAppSynchronize(domain);
}

// ---------- 网络 ----------

static NSString *UCParseLatest(NSString *text)
{
    if (text.length == 0) return nil;
    NSArray<NSString *> *lines = [text componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
    NSString *latest = nil;
    BOOL inPkg = NO;
    for (NSString *raw in lines) {
        NSString *l = [raw stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (l.length == 0) { inPkg = NO; continue; }
        if ([l hasPrefix:@"Package:"]) {
            NSString *pkg = [[l substringFromIndex:8] // "Package: <id>"，冒号后可能多空格
                stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            inPkg = [pkg hasPrefix:@"com.starmoor.sensedark"];
        } else if (inPkg && [l hasPrefix:@"Version:"]) {
            NSString *v = [[l substringFromIndex:8]
                stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            if (v.length && (!latest || SDVersionCompare(v, latest) > 0)) latest = v;
        }
    }
    return latest;
}

static void UCFetchLatest(void (^done)(NSString *latest))
{
    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:UCPackagesURL]];
    req.timeoutInterval = 15;

    NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    cfg.timeoutIntervalForRequest = 15;
    cfg.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    NSURLSession *sess = [NSURLSession sessionWithConfiguration:cfg];
    [[sess dataTaskWithRequest:req
             completionHandler:^(NSData *data, __unused NSURLResponse *resp, NSError *err) {
        NSString *latest = nil;
        if (err == nil && data.length > 0 && data.length < 262144) {
            latest = UCParseLatest([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]);
        }
        done(latest);
    }] resume];
    [sess finishTasksAndInvalidate];
}

static void UCSendPing(NSString *uid, void (^done)(BOOL ok))
{
    NSOperatingSystemVersion sv = [NSProcessInfo processInfo].operatingSystemVersion;
    NSString *os = [NSString stringWithFormat:@"ios-%ld.%ld", (long)sv.majorVersion, (long)sv.minorVersion];

    unsigned char mac[CC_SHA256_DIGEST_LENGTH];
    CCHmac(kCCHmacAlgSHA256, UC_SALT, strlen(UC_SALT),
           uid.UTF8String, strlen(uid.UTF8String), mac);
    char tag[17];
    for (int i = 0; i < 8; i++) snprintf(tag + i * 2, 3, "%02x", mac[i]);

    NSDictionary *body = @{
        @"app": @UC_APP,
        @"ver": @SD_VERSION,
        @"os": os,
        @"uid": uid,
        @"tag": [NSString stringWithFormat:@"%s", tag],
    };
    NSError *je = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:body options:0 error:&je];
    if (!data) {
        done(NO);
        return;
    }

    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:UCPingURL]];
    req.HTTPMethod = @"POST";
    req.HTTPBody = data;
    req.timeoutInterval = 10;
    [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];

    NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    cfg.timeoutIntervalForRequest = 10;
    NSURLSession *sess = [NSURLSession sessionWithConfiguration:cfg];
    [[sess dataTaskWithRequest:req
             completionHandler:^(__unused NSData *rsp, __unused NSURLResponse *resp, NSError *err) {
        done(err == nil); // 端点恒 204：拿到任何响应即送达
    }] resume];
    [sess finishTasksAndInvalidate];
}

// ---------- 状态机（后台串行队列） ----------

static dispatch_queue_t UCQueue(void)
{
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("com.starmoor.sensedark.uc", DISPATCH_QUEUE_SERIAL); });
    return q;
}

static BOOL gUCInFlight;

static void UCCheckFire(void);

static void UCScheduleAfter(int64_t sec)
{
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)sec * NSEC_PER_SEC), UCQueue(), ^{
        UCCheckFire();
    });
}

static void UCCheckFire(void)
{
    @autoreleasepool {
        NSDictionary *st = UCReadState();
        int64_t now = (int64_t)[[NSDate date] timeIntervalSince1970];

        NSString *uid = st[@"uc.uid"];
        if (uid.length == 0) {
            // 首启：生成随机 uid，首次检查定在 30min~3h 后的随机点
            unsigned char buf[16];
            arc4random_buf(buf, sizeof(buf));
            NSMutableString *u = [NSMutableString stringWithCapacity:32];
            for (int i = 0; i < 16; i++) [u appendFormat:@"%02x", buf[i]];
            UCWriteState(@{ @"uc.uid": u, @"uc.at": @(now + UCRandRange(1800, 3 * 3600)) });
            UCScheduleAfter(UCRandRange(2400, 4800)); // 下次醒来再核对是否到期
            return;
        }

        int64_t at = [st[@"uc.at"] longLongValue];
        if (now < at || gUCInFlight) {
            UCScheduleAfter(UCRandRange(2400, 4800)); // 40~80min 后醒来核对
            return;
        }

        gUCInFlight = YES;
        int64_t sentAt = now;
        UCFetchLatest(^(NSString *latest) {
            UCSendPing(uid, ^(BOOL ok) {
                NSMutableDictionary *w = [NSMutableDictionary dictionary];
                if (latest.length) w[@"uc.latest"] = latest;
                // 任一请求到达即视为在线：正常间隔 24~72h；全失败 1~3h 后重试
                BOOL online = latest.length > 0 || ok;
                w[@"uc.at"] = @(sentAt + (online ? UCRandRange(24 * 3600, 72 * 3600)
                                                : UCRandRange(3600, 3 * 3600)));
                UCWriteState(w);
                gUCInFlight = NO;
                UCScheduleAfter(UCRandRange(2400, 4800));
            });
        });
    }
}

void SDUpdateCheckStart(void)
{
    // 只挂 60s 后的首查（SB 先稳定），实际工作全在后台队列
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)60 * NSEC_PER_SEC), UCQueue(), ^{
        UCCheckFire();
    });
}
