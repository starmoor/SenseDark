// 「根据环境光」选项页内嵌面板：
//   - 3 组紧凑布局（实时状态 / 亮度阈值+多色滞回条 / 响应与省电），零冗余留白、完整保留提示
//   - SDHysteresisBarView：纯 CALayer 零位图多色区间条 + 实时光感指示线（60fps 联动）
//   - SDCompactSliderView：紧凑双层滑块（左标题 + 右可点数字胶囊 + 通栏滑块，零 AutoLayout 开销）
//   - 离开页面即销毁定时器、通知观察者与 notify 句柄，零常驻缓存
// 被 SenseDarkSettings.x 单一 TU include；要求先 include SDLog.h（Ping / PingFmt）。

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <notify.h>
#import <objc/runtime.h>
#import <objc/message.h>

#include "SDCommon.h"
#include "SDLoc.h"
#include "SDPSSpec.h"

static NSString *const kOurSpecID = @"SENSEDARK_AMBIENT";
static NSString *const kAmbientPrefix = @"SD_AMB_";
static NSString *const kRadioCheckedKey = @"radioGroupCheckedSpecifier";
static const NSInteger kHysteresisBarTag = 90211;
static const NSInteger kCompactSliderTag = 90212;

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

static int ReadPrefInt(NSString *key, int defVal)
{
    id v = ReadPrefObj(key);
    return [v respondsToSelector:@selector(intValue)] ? [v intValue] : defVal;
}

static double ReadPrefDouble(NSString *key, double defVal)
{
    id v = ReadPrefObj(key);
    if (![v respondsToSelector:@selector(doubleValue)]) return defVal;
    double d = [v doubleValue];
    return (d == d) ? d : defVal;
}

static int ReadScheduleMode(void)
{
    return ReadPrefInt(@SDKEY_SCHEDMODE, 0);
}

static int ReadLowPowerPolicy(void)
{
    int p = ReadPrefInt(@SDKEY_LP_POLICY, SDDEF_LP_POLICY);
    return (p < SDLP_KEEP || p > SDLP_PAUSE) ? SDDEF_LP_POLICY : p;
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

#pragma mark - 数值与格式化辅助

static inline double RoundOneDecimal(double v)
{
    return round(v * 10.0) / 10.0;
}

static inline double ClampDoubleVal(double v, double lo, double hi)
{
    if (!(v == v)) return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

static NSString *FormatNumberStr(double val, BOOL allowDecimal)
{
    if (!allowDecimal) {
        return [NSString stringWithFormat:@"%d", (int)lround(val)];
    }
    double r = RoundOneDecimal(val);
    if (fabs(r - round(r)) < 1e-6) {
        return [NSString stringWithFormat:@"%d", (int)lround(r)];
    }
    return [NSString stringWithFormat:@"%.1f", r];
}

static NSString *FormatSliderValue(double val, NSString *unit, BOOL allowDecimal)
{
    NSString *num = FormatNumberStr(val, allowDecimal);
    if ([unit isEqualToString:@"lux"]) {
        return [NSString stringWithFormat:@"%@ %@", num, SDLoc(@"lux", @"lux", @"лк")];
    }
    if ([unit isEqualToString:@"sec"]) {
        return [NSString stringWithFormat:@"%@ %@", num, SDLoc(@"秒", @"s", @"с")];
    }
    return num;
}

static NSString *LowPowerPolicyTitle(int policy)
{
    if (policy == SDLP_KEEP)  return SDLoc(@"保持不变", @"Keep Normal", @"Без изменений");
    if (policy == SDLP_PAUSE) return SDLoc(@"暂停切换", @"Pause", @"Пауза");
    return SDLoc(@"降频采样", @"Slow Down", @"Реже");
}

static BOOL IsLowPowerPausedNow(void)
{
    return [NSProcessInfo processInfo].isLowPowerModeEnabled && ReadLowPowerPolicy() == SDLP_PAUSE;
}

#pragma mark - 多色滞回阈值关系条（纯 CALayer 零位图内存）

@interface SDHysteresisBarView : UIView {
    UIView *_trackView;
    CALayer *_darkLayer;
    CALayer *_bufferLayer;
    CALayer *_lightLayer;
    CALayer *_indicatorLayer;
    UILabel *_leftLabel;
    UILabel *_midLabel;
    UILabel *_rightLabel;
    double _downLux;
    double _upLux;
    long long _liveLux; // <0 表示未就绪或已暂停
}
- (void)updateDownLux:(double)downLux upLux:(double)upLux liveLux:(long long)liveLux;
@end

@implementation SDHysteresisBarView

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    self.tag = kHysteresisBarTag;
    self.userInteractionEnabled = NO;
    self.backgroundColor = [UIColor clearColor];

    _trackView = [[UIView alloc] initWithFrame:CGRectZero];
    _trackView.layer.cornerRadius = 7.0;
    _trackView.clipsToBounds = YES;
    [self addSubview:_trackView];

    _darkLayer = [CALayer layer];
    _bufferLayer = [CALayer layer];
    _lightLayer = [CALayer layer];
    _indicatorLayer = [CALayer layer];
    _indicatorLayer.cornerRadius = 1.5;

    [_trackView.layer addSublayer:_darkLayer];
    [_trackView.layer addSublayer:_bufferLayer];
    [_trackView.layer addSublayer:_lightLayer];
    [_trackView.layer addSublayer:_indicatorLayer];

    UIFont *captionFont = [UIFont monospacedDigitSystemFontOfSize:11.5 weight:UIFontWeightRegular];
    UIColor *captionColor = [UIColor secondaryLabelColor];

    _leftLabel = [[UILabel alloc] initWithFrame:CGRectZero];
    _leftLabel.font = captionFont;
    _leftLabel.textColor = captionColor;
    _leftLabel.text = @"0";
    [self addSubview:_leftLabel];

    _midLabel = [[UILabel alloc] initWithFrame:CGRectZero];
    _midLabel.font = captionFont;
    _midLabel.textColor = captionColor;
    _midLabel.textAlignment = NSTextAlignmentCenter;
    [self addSubview:_midLabel];

    _rightLabel = [[UILabel alloc] initWithFrame:CGRectZero];
    _rightLabel.font = captionFont;
    _rightLabel.textColor = captionColor;
    _rightLabel.textAlignment = NSTextAlignmentRight;
    _rightLabel.text = [NSString stringWithFormat:@"%d+", (int)SDMAX_UP];
    [self addSubview:_rightLabel];

    _downLux = SDDEF_DOWN;
    _upLux = SDDEF_UP;
    _liveLux = -1;
    [self applyZoneColors];
    return self;
}

- (void)applyZoneColors
{
    BOOL isDark = (self.traitCollection.userInterfaceStyle == UIUserInterfaceStyleDark);
    UIColor *darkZone = isDark
        ? [UIColor colorWithRed:0.22 green:0.30 blue:0.56 alpha:1.0]
        : [UIColor colorWithRed:0.114 green:0.153 blue:0.310 alpha:1.0];
    UIColor *bufferZone = [[UIColor secondaryLabelColor] colorWithAlphaComponent:(isDark ? 0.30 : 0.22)];
    UIColor *lightZone = isDark
        ? [UIColor colorWithRed:0.83 green:0.70 blue:0.31 alpha:1.0]
        : [UIColor colorWithRed:0.906 green:0.788 blue:0.420 alpha:1.0];
    UIColor *indicator = [UIColor systemBlueColor];

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _darkLayer.backgroundColor = darkZone.CGColor;
    _bufferLayer.backgroundColor = bufferZone.CGColor;
    _lightLayer.backgroundColor = lightZone.CGColor;
    _indicatorLayer.backgroundColor = indicator.CGColor;
    [CATransaction commit];
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection
{
    [super traitCollectionDidChange:previousTraitCollection];
    [self applyZoneColors];
}

- (void)updateDownLux:(double)downLux upLux:(double)upLux liveLux:(long long)liveLux
{
    _downLux = ClampDoubleVal(downLux, SDMIN_DOWN, SDMAX_DOWN);
    _upLux = ClampDoubleVal(upLux, SDMIN_UP, SDMAX_UP);
    if (_downLux > _upLux - SDMIN_LUX_GAP) {
        _downLux = ClampDoubleVal(_upLux - SDMIN_LUX_GAP, SDMIN_DOWN, SDMAX_DOWN);
    }
    _liveLux = liveLux;

    NSString *dStr = FormatNumberStr(_downLux, YES);
    NSString *uStr = FormatNumberStr(_upLux, YES);
    _midLabel.text = [NSString stringWithFormat:
        SDLoc(@"深 ≤%@  |  浅 ≥%@", @"Dark ≤%@  |  Light ≥%@", @"Темн. ≤%@  |  Светл. ≥%@"),
        dStr, uStr];
    [self layoutLayersOnly];
}

- (void)layoutLayersOnly
{
    CGFloat w = _trackView.bounds.size.width;
    CGFloat h = _trackView.bounds.size.height;
    if (w <= 1.0 || h <= 1.0) return;

    double maxScale = (double)SDMAX_UP;
    CGFloat downX = (CGFloat)((_downLux / maxScale) * w);
    if (_downLux > 0.0 && downX < 3.0) downX = 3.0;
    CGFloat upX = (CGFloat)((_upLux / maxScale) * w);
    if (upX < downX + 2.0) upX = fmin(w, downX + 2.0);

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _darkLayer.frame = CGRectMake(0, 0, downX, h);
    _bufferLayer.frame = CGRectMake(downX, 0, fmax(0.0, upX - downX), h);
    _lightLayer.frame = CGRectMake(upX, 0, fmax(0.0, w - upX), h);

    if (_liveLux >= 0) {
        double clamped = (_liveLux > SDMAX_UP) ? (double)SDMAX_UP : (double)_liveLux;
        CGFloat indW = 3.0;
        CGFloat luxX = (CGFloat)((clamped / maxScale) * w);
        CGFloat x = fmax(0.0, fmin(w - indW, luxX - indW * 0.5));
        _indicatorLayer.hidden = NO;
        _indicatorLayer.frame = CGRectMake(x, 0, indW, h);
    } else {
        _indicatorLayer.hidden = YES;
    }
    [CATransaction commit];
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    CGFloat pad = 16.0;
    CGFloat w = fmax(0.0, self.bounds.size.width - pad * 2.0);
    _trackView.frame = CGRectMake(pad, 12.0, w, 14.0);
    CGFloat ly = 31.0, lh = 16.0;
    _leftLabel.frame = CGRectMake(pad, ly, 36.0, lh);
    _rightLabel.frame = CGRectMake(pad + w - 42.0, ly, 42.0, lh);
    _midLabel.frame = CGRectMake(pad + 36.0, ly, fmax(0.0, w - 78.0), lh);
    [self layoutLayersOnly];
}

@end

#pragma mark - 紧凑双层滑块视图（左标题 + 右可点击数值胶囊 + 通栏滑块，零 AutoLayout 开销）

@interface SDCompactSliderView : UIView
@property (nonatomic, copy) NSString *specID;
@property (nonatomic, strong, readonly) UILabel *titleLabel;
@property (nonatomic, strong, readonly) UIButton *valueButton;
@property (nonatomic, strong, readonly) UISlider *slider;
@end

@implementation SDCompactSliderView

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    self.tag = kCompactSliderTag;
    self.backgroundColor = [UIColor clearColor];

    _titleLabel = [[UILabel alloc] initWithFrame:CGRectZero];
    _titleLabel.font = [UIFont systemFontOfSize:15.0 weight:UIFontWeightRegular];
    _titleLabel.textColor = [UIColor labelColor];
    _titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [self addSubview:_titleLabel];

    _valueButton = [UIButton buttonWithType:UIButtonTypeSystem];
    _valueButton.titleLabel.font = [UIFont monospacedDigitSystemFontOfSize:14.0 weight:UIFontWeightMedium];
    _valueButton.backgroundColor = [UIColor secondarySystemFillColor];
    _valueButton.layer.cornerRadius = 7.0;
    _valueButton.clipsToBounds = YES;
    [self addSubview:_valueButton];

    _slider = [[UISlider alloc] initWithFrame:CGRectZero];
    _slider.continuous = YES;
    [self addSubview:_slider];
    return self;
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    CGFloat pad = 16.0;
    CGFloat w = self.bounds.size.width;
    CGFloat btnW = 76.0, btnH = 26.0;
    _valueButton.frame = CGRectMake(w - pad - btnW, 8.0, btnW, btnH);
    _titleLabel.frame = CGRectMake(pad, 8.0, fmax(0.0, w - pad * 2.0 - btnW - 8.0), btnH);
    _slider.frame = CGRectMake(pad, 36.0, fmax(0.0, w - pad * 2.0), 28.0);
}

@end

#pragma mark - 实时状态与跨进程同步

static long long sLiveLux = -1; // -1=尚未读到
static BOOL sPausedByManual = NO;
static __weak id sLiveController = nil;
static dispatch_source_t sNotifyPollTimer = nil;
static id sLowPowerObserver = nil;
static int sLuxNotifyToken = -1;
static int sStateNotifyToken = -1;

static void SyncResumeButtonRow(id controller, BOOL show, BOOL animated);
static void RefreshHysteresisBarVisual(id ctrl, double downLux, double upLux);
static double CurrentLiveUpLux(id ctrl);
static double CurrentLiveDownLux(id ctrl);

static NSString *FormatCurrentStatus(void)
{
    if (IsLowPowerPausedNow()) {
        return SDLoc(@"低电量已暂停", @"Paused (Low Power)", @"Пауза (Энергосбережение)");
    }
    if (sPausedByManual) {
        return SDLoc(@"已暂停（手动切换）", @"Paused (Manual Switch)", @"Пауза (ручная смена)");
    }
    if ([NSProcessInfo processInfo].isLowPowerModeEnabled && ReadLowPowerPolicy() == SDLP_SLOW) {
        int normalIv = ReadPrefInt(@SDKEY_INTERVAL, SDDEF_INTERVAL);
        int lpIv = ReadPrefInt(@SDKEY_LP_INTERVAL, SDDEF_LP_INTERVAL);
        int eff = lpIv > normalIv ? lpIv : normalIv;
        return [NSString stringWithFormat:
            SDLoc(@"低电量降频（%d 秒）", @"Low Power (%ds)", @"Экономия (%dс)"), eff];
    }
    return SDLoc(@"运行中", @"Active", @"Активно");
}

static NSString *FormatLiveLux(long long lux)
{
    if (IsLowPowerPausedNow() || lux < 0) return @"--";
    return [NSString stringWithFormat:@"%lld %@", lux, SDLoc(@"lux", @"lux", @"лк")];
}

static UITableViewCell *VisibleCellForSpecID(id ctrl, NSString *specID)
{
    if (!ctrl || !specID) return nil;
    id sp = SpecForID(ctrl, specID);
    if (!sp) return nil;
    NSIndexPath *ip = IndexPathForSpec(ctrl, sp);
    if (!ip) return nil;
    UITableView *tv = [ctrl respondsToSelector:@selector(table)]
        ? ((UITableView *(*)(id, SEL))objc_msgSend)(ctrl, @selector(table))
        : nil;
    return tv ? [tv cellForRowAtIndexPath:ip] : nil;
}

static void RefreshDetailTextForSpecID(id ctrl, NSString *specID, NSString *text)
{
    UITableViewCell *cell = VisibleCellForSpecID(ctrl, specID);
    if (cell) {
        cell.detailTextLabel.text = text;
        [cell setNeedsLayout];
    }
}

static void RefreshHysteresisBarVisual(id ctrl, double downLux, double upLux)
{
    UITableViewCell *cell = VisibleCellForSpecID(ctrl, @"SD_AMB_BAR");
    if (!cell) return;
    SDHysteresisBarView *bar = (SDHysteresisBarView *)[cell.contentView viewWithTag:kHysteresisBarTag];
    if ([bar isKindOfClass:[SDHysteresisBarView class]]) {
        [bar updateDownLux:downLux upLux:upLux liveLux:(IsLowPowerPausedNow() ? -1 : sLiveLux)];
    }
}

static void RefreshLiveStatusCellsOnMain(void)
{
    id ctrl = sLiveController;
    if (!ctrl) return;
    RefreshDetailTextForSpecID(ctrl, @"SD_AMB_LUX", FormatLiveLux(sLiveLux));
    RefreshDetailTextForSpecID(ctrl, @"SD_AMB_STATUS", FormatCurrentStatus());
    RefreshHysteresisBarVisual(ctrl, CurrentLiveDownLux(ctrl), CurrentLiveUpLux(ctrl));
    SyncResumeButtonRow(ctrl, sPausedByManual && !IsLowPowerPausedNow(), YES);
}

static void OnLiveLuxSample(long long lux)
{
    if (lux < 0) lux = 0;
    if (sLiveLux == lux) return;
    sLiveLux = lux;
    dispatch_block_t upd = ^{
        id ctrl = sLiveController;
        if (!ctrl) return;
        RefreshDetailTextForSpecID(ctrl, @"SD_AMB_LUX", FormatLiveLux(sLiveLux));
        RefreshHysteresisBarVisual(ctrl, CurrentLiveDownLux(ctrl), CurrentLiveUpLux(ctrl));
    };
    if ([NSThread isMainThread]) upd();
    else dispatch_async(dispatch_get_main_queue(), upd);
}

static void PollSpringBoardStateOnce(void)
{
    if (sStateNotifyToken < 0) {
        notify_register_check(SD_NOTE_STATE, &sStateNotifyToken);
    }
    if (sStateNotifyToken < 0) return;
    uint64_t bits = 0;
    if (notify_get_state(sStateNotifyToken, &bits) == NOTIFY_STATUS_OK) {
        BOOL paused = (bits & SDSTATE_PAUSED_MANUAL) != 0;
        if (paused != sPausedByManual) {
            sPausedByManual = paused;
            if ([NSThread isMainThread]) RefreshLiveStatusCellsOnMain();
            else dispatch_async(dispatch_get_main_queue(), ^{ RefreshLiveStatusCellsOnMain(); });
        }
    }
}

static void PollSpringBoardLuxOnce(void)
{
    if (sLuxNotifyToken < 0) {
        notify_register_check(SD_NOTE_LUX, &sLuxNotifyToken);
    }
    if (sLuxNotifyToken >= 0) {
        uint64_t state = 0;
        if (notify_get_state(sLuxNotifyToken, &state) == NOTIFY_STATUS_OK && state > 0) {
            OnLiveLuxSample((long long)(state - 1));
        }
    }
    PollSpringBoardStateOnce();
}

static void StopLiveLuxMonitor(void)
{
    if (sNotifyPollTimer) {
        dispatch_source_cancel(sNotifyPollTimer);
        sNotifyPollTimer = nil;
    }
    if (sLowPowerObserver) {
        [[NSNotificationCenter defaultCenter] removeObserver:sLowPowerObserver];
        sLowPowerObserver = nil;
    }
    if (sLuxNotifyToken >= 0) {
        notify_cancel(sLuxNotifyToken);
        sLuxNotifyToken = -1;
    }
    if (sStateNotifyToken >= 0) {
        notify_cancel(sStateNotifyToken);
        sStateNotifyToken = -1;
    }
    sLiveController = nil;
}

static void StartLiveLuxMonitor(id controller)
{
    sLiveController = controller;
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

    if (!sLowPowerObserver) {
        sLowPowerObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSProcessInfoPowerStateDidChangeNotification
                        object:nil
                         queue:nil
                    usingBlock:^(__unused NSNotification *note) {
            dispatch_async(dispatch_get_main_queue(), ^{ RefreshLiveStatusCellsOnMain(); });
        }];
    }
}

#pragma mark - 滑块查找与 UI 同步

static SDCompactSliderView *CompactSliderViewInCell(UITableViewCell *cell)
{
    if (!cell) return nil;
    UIView *v = [cell.contentView viewWithTag:kCompactSliderTag];
    return [v isKindOfClass:[SDCompactSliderView class]] ? (SDCompactSliderView *)v : nil;
}

static void UpdateCompactSliderButton(SDCompactSliderView *sv, double val, NSString *unit, BOOL allowDecimal)
{
    if (!sv) return;
    NSString *title = FormatSliderValue(val, unit, allowDecimal);
    [UIView performWithoutAnimation:^{
        [sv.valueButton setTitle:title forState:UIControlStateNormal];
        [sv.valueButton layoutIfNeeded];
    }];
}

static void SyncSliderCellVisual(id ctrl, NSString *specID, double val, NSString *unit, BOOL allowDecimal)
{
    SDCompactSliderView *sv = CompactSliderViewInCell(VisibleCellForSpecID(ctrl, specID));
    if (!sv) return;
    if (fabs((double)sv.slider.value - val) > 1e-4) {
        sv.slider.value = (float)val;
    }
    UpdateCompactSliderButton(sv, val, unit, allowDecimal);
}

static double CurrentLiveUpLux(id ctrl)
{
    SDCompactSliderView *sv = CompactSliderViewInCell(VisibleCellForSpecID(ctrl, @"SD_AMB_UP"));
    if (sv) return ClampDoubleVal(RoundOneDecimal(sv.slider.value), SDMIN_UP, SDMAX_UP);
    return ClampDoubleVal(RoundOneDecimal(ReadPrefDouble(@SDKEY_UP, SDDEF_UP)), SDMIN_UP, SDMAX_UP);
}

static double CurrentLiveDownLux(id ctrl)
{
    SDCompactSliderView *sv = CompactSliderViewInCell(VisibleCellForSpecID(ctrl, @"SD_AMB_DOWN"));
    if (sv) return ClampDoubleVal(RoundOneDecimal(sv.slider.value), SDMIN_DOWN, SDMAX_DOWN);
    return ClampDoubleVal(RoundOneDecimal(ReadPrefDouble(@SDKEY_DOWN, SDDEF_DOWN)), SDMIN_DOWN, SDMAX_DOWN);
}

static double CurrentMaxAllowedDownLux(id ctrl)
{
    double curUp = CurrentLiveUpLux(ctrl);
    return ClampDoubleVal(RoundOneDecimal(curUp - SDMIN_LUX_GAP), SDMIN_DOWN, SDMAX_DOWN);
}

static void ReloadSpecifierAnimated(id controller, id sp, BOOL animated)
{
    if (controller && sp && [controller respondsToSelector:@selector(reloadSpecifier:animated:)]) {
        ((void (*)(id, SEL, id, BOOL))objc_msgSend)(controller, @selector(reloadSpecifier:animated:), sp, animated);
    }
}

static void SyncLowPowerIntervalRow(id controller, BOOL show, BOOL animated);

#pragma mark - 环境光设置项 Target

@interface SDAmbientPrefsTarget : NSObject
+ (instancetype)sharedTarget;
- (id)getLiveLuxForSpecifier:(id)specifier;
- (id)getStatusForSpecifier:(id)specifier;
- (id)getLowPowerPolicyForSpecifier:(id)specifier;
- (id)getAmbientPrefForSpecifier:(id)specifier;
- (void)setAmbientPref:(id)value specifier:(id)specifier;
- (void)onSliderDragging:(UISlider *)slider;
- (void)onSliderTouchEnded:(UISlider *)slider;
- (void)onTapSliderValueButton:(UIButton *)btn;
@end

@implementation SDAmbientPrefsTarget

+ (instancetype)sharedTarget
{
    static SDAmbientPrefsTarget *inst;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ inst = [SDAmbientPrefsTarget new]; });
    return inst;
}

- (id)getLiveLuxForSpecifier:(id)specifier { return FormatLiveLux(sLiveLux); }
- (id)getStatusForSpecifier:(id)specifier { return FormatCurrentStatus(); }
- (id)getLowPowerPolicyForSpecifier:(id)specifier { return LowPowerPolicyTitle(ReadLowPowerPolicy()); }

- (double)readSliderValueForSpecifier:(id)specifier
{
    NSString *key = SpecProp(specifier, @"key");
    double defVal = [SpecProp(specifier, @"default") doubleValue];
    BOOL allowDec = [SpecProp(specifier, @"sdDecimal") boolValue];
    double minV = [SpecProp(specifier, @"min") doubleValue];
    double maxV = [SpecProp(specifier, @"max") doubleValue];
    double raw = key ? ReadPrefDouble(key, defVal) : defVal;
    double v = allowDec ? RoundOneDecimal(raw) : round(raw);
    v = ClampDoubleVal(v, minV, maxV);
    if ([key isEqualToString:@SDKEY_DOWN]) {
        double upVal = ClampDoubleVal(RoundOneDecimal(ReadPrefDouble(@SDKEY_UP, SDDEF_UP)), SDMIN_UP, SDMAX_UP);
        double maxDown = ClampDoubleVal(RoundOneDecimal(upVal - SDMIN_LUX_GAP), SDMIN_DOWN, SDMAX_DOWN);
        if (v > maxDown) v = maxDown;
    }
    return v;
}

- (id)getAmbientPrefForSpecifier:(id)specifier
{
    NSString *key = SpecProp(specifier, @"key");
    id defVal = SpecProp(specifier, @"default");
    id raw = key ? ReadPrefObj(key) : nil;
    return @([(raw ?: defVal) boolValue]);
}

- (void)setAmbientPref:(id)value specifier:(id)specifier
{
    NSString *key = SpecProp(specifier, @"key");
    if (!key) return;
    NSNumber *toWrite = @([value boolValue]);
    WriteKey(key, toWrite);
    PostPrefsChanged();
    PingFmt("st-pref-set?k=%s&v=%d", key.UTF8String, [toWrite intValue]);
}

// 统一钳制 + 双向联动 + 写盘广播 + 同步滑块与多色关系条
- (void)commitSliderValue:(double)rawVal forSpecifier:(id)specifier
{
    NSString *key = SpecProp(specifier, @"key");
    NSString *ident = SpecIdent(specifier);
    if (!key) return;

    BOOL allowDec = [SpecProp(specifier, @"sdDecimal") boolValue];
    NSString *unit = SpecProp(specifier, @"sdUnit");
    double minV = [SpecProp(specifier, @"min") doubleValue];
    double maxV = [SpecProp(specifier, @"max") doubleValue];
    double val = allowDec ? RoundOneDecimal(rawVal) : round(rawVal);
    val = ClampDoubleVal(val, minV, maxV);

    id ctrl = sLiveController;

    if ([key isEqualToString:@SDKEY_UP]) {
        double curDown = ClampDoubleVal(RoundOneDecimal(ReadPrefDouble(@SDKEY_DOWN, SDDEF_DOWN)), SDMIN_DOWN, SDMAX_DOWN);
        double maxDown = ClampDoubleVal(RoundOneDecimal(val - SDMIN_LUX_GAP), SDMIN_DOWN, SDMAX_DOWN);
        if (curDown > maxDown + 1e-6) {
            WriteKey(@SDKEY_DOWN, @(maxDown));
            if (ctrl) SyncSliderCellVisual(ctrl, @"SD_AMB_DOWN", maxDown, @"lux", YES);
        }
    } else if ([key isEqualToString:@SDKEY_DOWN]) {
        double maxDown = CurrentMaxAllowedDownLux(ctrl);
        if (val > maxDown + 1e-6) val = maxDown;
    }

    NSNumber *toWrite = allowDec ? @(val) : @((int)lround(val));
    WriteKey(key, toWrite);
    PostPrefsChanged();
    PingFmt("st-pref-set?k=%s&v=%.1f", key.UTF8String, val);

    if (ctrl) {
        SyncSliderCellVisual(ctrl, ident, val, unit, allowDec);
        if ([key isEqualToString:@SDKEY_UP] || [key isEqualToString:@SDKEY_DOWN]) {
            RefreshHysteresisBarVisual(ctrl, CurrentLiveDownLux(ctrl), CurrentLiveUpLux(ctrl));
        } else if ([key isEqualToString:@SDKEY_INTERVAL] || [key isEqualToString:@SDKEY_LP_INTERVAL]) {
            RefreshDetailTextForSpecID(ctrl, @"SD_AMB_STATUS", FormatCurrentStatus());
        }
    }
}

// 拖动时 60fps 实时更新数字胶囊、联动限制浅/深阈值并实时重绘多色条（零写盘）
- (void)onSliderDragging:(UISlider *)slider
{
    SDCompactSliderView *sv = [slider.superview isKindOfClass:[SDCompactSliderView class]]
        ? (SDCompactSliderView *)slider.superview : nil;
    if (!sv || !sv.specID) return;

    id ctrl = sLiveController;
    id sp = SpecForID(ctrl, sv.specID);
    if (!sp) return;

    BOOL allowDec = [SpecProp(sp, @"sdDecimal") boolValue];
    NSString *unit = SpecProp(sp, @"sdUnit");
    double val = allowDec ? RoundOneDecimal(slider.value) : round(slider.value);

    if ([sv.specID isEqualToString:@"SD_AMB_DOWN"]) {
        double maxDown = CurrentMaxAllowedDownLux(ctrl);
        if (val > maxDown + 1e-6) {
            val = maxDown;
            slider.value = (float)val;
        }
        UpdateCompactSliderButton(sv, val, unit, allowDec);
        RefreshHysteresisBarVisual(ctrl, val, CurrentLiveUpLux(ctrl));
        return;
    }

    if ([sv.specID isEqualToString:@"SD_AMB_UP"]) {
        double maxDown = ClampDoubleVal(RoundOneDecimal(val - SDMIN_LUX_GAP), SDMIN_DOWN, SDMAX_DOWN);
        double origDown = ClampDoubleVal(RoundOneDecimal(ReadPrefDouble(@SDKEY_DOWN, SDDEF_DOWN)), SDMIN_DOWN, SDMAX_DOWN);
        double visualDown = origDown > maxDown ? maxDown : origDown;
        SyncSliderCellVisual(ctrl, @"SD_AMB_DOWN", visualDown, @"lux", YES);
        UpdateCompactSliderButton(sv, val, unit, allowDec);
        RefreshHysteresisBarVisual(ctrl, visualDown, val);
        return;
    }

    UpdateCompactSliderButton(sv, val, unit, allowDec);
}

- (void)onSliderTouchEnded:(UISlider *)slider
{
    SDCompactSliderView *sv = [slider.superview isKindOfClass:[SDCompactSliderView class]]
        ? (SDCompactSliderView *)slider.superview : nil;
    if (!sv || !sv.specID) return;
    id sp = SpecForID(sLiveController, sv.specID);
    if (sp) [self commitSliderValue:slider.value forSpecifier:sp];
}

- (void)onTapSliderValueButton:(UIButton *)btn
{
    id ctrl = sLiveController;
    if (![ctrl isKindOfClass:[UIViewController class]]) return;
    SDCompactSliderView *sv = [btn.superview isKindOfClass:[SDCompactSliderView class]]
        ? (SDCompactSliderView *)btn.superview : nil;
    if (!sv || !sv.specID) return;
    id sp = SpecForID(ctrl, sv.specID);
    if (!sp) return;

    BOOL allowDec = [SpecProp(sp, @"sdDecimal") boolValue];
    NSString *title = SpecProp(sp, @"sdTitle") ?: @"";
    double minV = [SpecProp(sp, @"min") doubleValue];
    double maxV = [SpecProp(sp, @"max") doubleValue];
    if ([sv.specID isEqualToString:@"SD_AMB_DOWN"]) {
        maxV = CurrentMaxAllowedDownLux(ctrl);
    }
    double curV = [self readSliderValueForSpecifier:sp];

    NSString *hint = [NSString stringWithFormat:
        SDLoc(@"允许范围：%@ – %@", @"Allowed range: %@ – %@", @"Допустимый диапазон: %@ – %@"),
        FormatNumberStr(minV, allowDec), FormatNumberStr(maxV, allowDec)];

    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title
                                                                   message:hint
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *tf) {
        tf.keyboardType = allowDec ? UIKeyboardTypeDecimalPad : UIKeyboardTypeNumberPad;
        tf.text = FormatNumberStr(curV, allowDec);
        tf.clearButtonMode = UITextFieldViewModeWhileEditing;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:SDLoc(@"取消", @"Cancel", @"Отмена")
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    __weak typeof(self) weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:SDLoc(@"确定", @"OK", @"ОК")
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        NSString *txt = [alert.textFields.firstObject.text
            stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (!txt.length) return;
        txt = [txt stringByReplacingOccurrencesOfString:@"," withString:@"."];
        NSScanner *sc = [NSScanner scannerWithString:txt];
        double parsed = 0;
        if ([sc scanDouble:&parsed] && sc.isAtEnd) {
            [weakSelf commitSliderValue:parsed forSpecifier:sp];
        }
    }]];
    [(UIViewController *)ctrl presentViewController:alert animated:YES completion:nil];
}

@end

#pragma mark - Cell 配置与高度

static void CleanupCustomAmbientViews(UITableViewCell *cell)
{
    if (!cell) return;
    UIView *bar = [cell.contentView viewWithTag:kHysteresisBarTag];
    if (bar) [bar removeFromSuperview];
    UIView *sl = [cell.contentView viewWithTag:kCompactSliderTag];
    if (sl) [sl removeFromSuperview];
}

static CGFloat HeightForAmbientSpecifier(id specifier)
{
    NSString *kind = SpecProp(specifier, @"sdKind");
    if ([kind isEqualToString:@"bar"]) return 56.0;
    if ([kind isEqualToString:@"slider"]) return 68.0;
    return -1.0; // 使用原生默认行高
}

static void ConfigureAmbientCell(UITableViewCell *cell, id specifier)
{
    if (!cell || !specifier) return;
    NSString *ident = SpecIdent(specifier);
    NSString *kind = SpecProp(specifier, @"sdKind");

    if ([kind isEqualToString:@"bar"]) {
        UIView *oldSl = [cell.contentView viewWithTag:kCompactSliderTag];
        if (oldSl) [oldSl removeFromSuperview];

        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        cell.accessoryType = UITableViewCellAccessoryNone;
        cell.textLabel.text = nil;
        cell.detailTextLabel.text = nil;

        SDHysteresisBarView *bar = (SDHysteresisBarView *)[cell.contentView viewWithTag:kHysteresisBarTag];
        if (![bar isKindOfClass:[SDHysteresisBarView class]]) {
            bar = [[SDHysteresisBarView alloc] initWithFrame:cell.contentView.bounds];
            bar.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            [cell.contentView addSubview:bar];
        }
        double upV = ClampDoubleVal(RoundOneDecimal(ReadPrefDouble(@SDKEY_UP, SDDEF_UP)), SDMIN_UP, SDMAX_UP);
        double downV = ClampDoubleVal(RoundOneDecimal(ReadPrefDouble(@SDKEY_DOWN, SDDEF_DOWN)), SDMIN_DOWN, SDMAX_DOWN);
        [bar updateDownLux:downV upLux:upV liveLux:(IsLowPowerPausedNow() ? -1 : sLiveLux)];
        return;
    }

    if ([kind isEqualToString:@"slider"]) {
        UIView *oldBar = [cell.contentView viewWithTag:kHysteresisBarTag];
        if (oldBar) [oldBar removeFromSuperview];

        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        cell.accessoryType = UITableViewCellAccessoryNone;
        cell.textLabel.text = nil;
        cell.detailTextLabel.text = nil;

        SDCompactSliderView *sv = CompactSliderViewInCell(cell);
        SDAmbientPrefsTarget *target = [SDAmbientPrefsTarget sharedTarget];
        if (!sv) {
            sv = [[SDCompactSliderView alloc] initWithFrame:cell.contentView.bounds];
            sv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            [sv.valueButton addTarget:target action:@selector(onTapSliderValueButton:) forControlEvents:UIControlEventTouchUpInside];
            [sv.slider addTarget:target action:@selector(onSliderDragging:) forControlEvents:UIControlEventValueChanged];
            UIControlEvents endEvs = UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel;
            [sv.slider addTarget:target action:@selector(onSliderTouchEnded:) forControlEvents:endEvs];
            [cell.contentView addSubview:sv];
        }

        sv.specID = ident;
        sv.titleLabel.text = SpecProp(specifier, @"sdTitle") ?: @"";
        sv.slider.minimumValue = [SpecProp(specifier, @"min") floatValue];
        sv.slider.maximumValue = [SpecProp(specifier, @"max") floatValue];
        double curVal = [target readSliderValueForSpecifier:specifier];
        sv.slider.value = (float)curVal;
        UpdateCompactSliderButton(sv, curVal, SpecProp(specifier, @"sdUnit"), [SpecProp(specifier, @"sdDecimal") boolValue]);
        return;
    }

    CleanupCustomAmbientViews(cell);
    if ([ident isEqualToString:@"SD_AMB_LP_POLICY"]) {
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    } else if (SpecCellType(specifier) == kSDTitleValueCell) {
        cell.accessoryType = UITableViewCellAccessoryNone;
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
    }
}

#pragma mark - Specifier 构造（3 组紧凑排布）

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
    if (footer.length) SpecSetProperty(g, footer, @"footerText");
    return g;
}

static id MakeBarSpecifier(void)
{
    Class specCls = objc_getClass("PSSpecifier");
    if (!specCls) return nil;
    id sp = ((id (*)(Class, SEL, id, id, SEL, SEL, Class, long, BOOL))objc_msgSend)(
        specCls,
        @selector(preferenceSpecifierNamed:target:set:get:detail:cell:edit:),
        @"", nil, NULL, NULL, Nil, kSDTitleValueCell, NO);
    if (!sp) return nil;
    SpecSetProperty(sp, @"SD_AMB_BAR", @"id");
    SpecSetProperty(sp, @"bar", @"sdKind");
    return sp;
}

static id MakeCompactSliderSpecifier(NSString *ident, NSString *title, NSString *key, NSString *unit,
                                     double minVal, double maxVal, double defVal, BOOL allowDecimal)
{
    Class specCls = objc_getClass("PSSpecifier");
    if (!specCls) return nil;
    id sp = ((id (*)(Class, SEL, id, id, SEL, SEL, Class, long, BOOL))objc_msgSend)(
        specCls,
        @selector(preferenceSpecifierNamed:target:set:get:detail:cell:edit:),
        @"", nil, NULL, NULL, Nil, kSDTitleValueCell, NO);
    if (!sp) return nil;
    SpecSetProperty(sp, ident, @"id");
    SpecSetProperty(sp, @"slider", @"sdKind");
    SpecSetProperty(sp, key, @"key");
    SpecSetProperty(sp, @(minVal), @"min");
    SpecSetProperty(sp, @(maxVal), @"max");
    SpecSetProperty(sp, @(defVal), @"default");
    SpecSetProperty(sp, @(allowDecimal), @"sdDecimal");
    if (unit) SpecSetProperty(sp, unit, @"sdUnit");
    if (title) SpecSetProperty(sp, title, @"sdTitle");
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
        title, target, @selector(setAmbientPref:specifier:), @selector(getAmbientPrefForSpecifier:),
        Nil, kSDSwitchCell, NO);
    if (!sp) return nil;
    SpecSetProperty(sp, ident, @"id");
    SpecSetProperty(sp, key, @"key");
    SpecSetProperty(sp, @(SD_PREFS_DOMAIN), @"defaults");
    SpecSetProperty(sp, @(defVal), @"default");
    return sp;
}

static id MakeTitleValueSpecifier(NSString *ident, NSString *title, SEL getter)
{
    Class specCls = objc_getClass("PSSpecifier");
    if (!specCls) return nil;
    id target = [SDAmbientPrefsTarget sharedTarget];
    id sp = ((id (*)(Class, SEL, id, id, SEL, SEL, Class, long, BOOL))objc_msgSend)(
        specCls,
        @selector(preferenceSpecifierNamed:target:set:get:detail:cell:edit:),
        title, target, NULL, getter, Nil, kSDTitleValueCell, NO);
    if (!sp) return nil;
    SpecSetProperty(sp, ident, @"id");
    return sp;
}

static id MakeResumeButtonSpecifier(void)
{
    Class specCls = objc_getClass("PSSpecifier");
    if (!specCls) return nil;
    id sp = ((id (*)(Class, SEL, id, id, SEL, SEL, Class, long, BOOL))objc_msgSend)(
        specCls,
        @selector(preferenceSpecifierNamed:target:set:get:detail:cell:edit:),
        SDLoc(@"立即恢复", @"Resume Now", @"Возобновить сейчас"),
        nil, NULL, NULL, Nil, 13, NO);
    if (!sp) return nil;
    SpecSetProperty(sp, @"SD_AMB_RESUME", @"id");
    return sp;
}

static id MakeLowPowerIntervalSpecifier(void)
{
    return MakeCompactSliderSpecifier(
        @"SD_AMB_LP_INTERVAL",
        SDLoc(@"低电量采样间隔", @"Low Power Interval", @"Интервал при экономии"),
        @SDKEY_LP_INTERVAL, @"sec", SDMIN_LP_INTERVAL, SDMAX_LP_INTERVAL, SDDEF_LP_INTERVAL, NO);
}

static void SyncResumeButtonRow(id controller, BOOL show, BOOL animated)
{
    if (!controller) return;
    id existing = SpecForID(controller, @"SD_AMB_RESUME");
    if (show == (existing != nil)) return;

    if (show) {
        id btn = MakeResumeButtonSpecifier();
        if (btn &&
            [controller respondsToSelector:@selector(insertContiguousSpecifiers:afterSpecifierID:animated:)] &&
            SpecForID(controller, @"SD_AMB_STATUS")) {
            ((void (*)(id, SEL, id, id, BOOL))objc_msgSend)(
                controller, @selector(insertContiguousSpecifiers:afterSpecifierID:animated:),
                @[btn], @"SD_AMB_STATUS", animated);
        }
    } else if (existing && [controller respondsToSelector:@selector(removeContiguousSpecifiers:animated:)]) {
        ((void (*)(id, SEL, id, BOOL))objc_msgSend)(
            controller, @selector(removeContiguousSpecifiers:animated:), @[existing], animated);
    }
}

static NSArray *BuildAmbientSpecifiers(void)
{
    NSMutableArray *arr = [NSMutableArray arrayWithCapacity:14];
    void (^add)(id) = ^(id sp) { if (sp) [arr addObject:sp]; };

    // 组 1：实时环境光与状态
    add(MakeGroupSpecifier(@"SD_AMB_GRP_LUX", @"", nil));
    add(MakeTitleValueSpecifier(
        @"SD_AMB_LUX",
        SDLoc(@"当前环境光", @"Current Ambient Light", @"Текущая освещенность"),
        @selector(getLiveLuxForSpecifier:)));
    add(MakeTitleValueSpecifier(
        @"SD_AMB_STATUS",
        SDLoc(@"状态", @"Status", @"Статус"),
        @selector(getStatusForSpecifier:)));
    if (sPausedByManual && !IsLowPowerPausedNow()) {
        add(MakeResumeButtonSpecifier());
    }

    // 组 2：亮度阈值（多色滞回关系条 + 浅色/深色滑块紧凑同组）
    add(MakeGroupSpecifier(
        @"SD_AMB_GRP_THRESH",
        SDLoc(@"亮度阈值", @"Brightness Thresholds", @"Пороги освещенности"),
        SDLoc(@"≥浅色阈值切浅色，≤深色阈值切深色（暗室约 0–10，室内约 15–50），中间为防闪烁缓冲。点击数值可手动输入。",
              @"Light at ≥Light, Dark at ≤Dark (dark room ~0–10, indoor ~15–50), with hysteresis in between. Tap any value to type.",
              @"Светлая при ≥светлого, темная при ≤темного (темно ~0–10, в помещении ~15–50), между ними буфер. Нажмите на число для ввода.")));
    add(MakeBarSpecifier());
    add(MakeCompactSliderSpecifier(
        @"SD_AMB_UP",
        SDLoc(@"浅色阈值", @"Light Threshold", @"Порог светлого"),
        @SDKEY_UP, @"lux", SDMIN_UP, SDMAX_UP, SDDEF_UP, YES));
    add(MakeCompactSliderSpecifier(
        @"SD_AMB_DOWN",
        SDLoc(@"深色阈值", @"Dark Threshold", @"Порог темного"),
        @SDKEY_DOWN, @"lux", SDMIN_DOWN, SDMAX_DOWN, SDDEF_DOWN, YES));

    // 组 3：响应与省电（延迟 + 采样 + 低电量策略/间隔 + 手动暂停 紧凑同组）
    add(MakeGroupSpecifier(
        @"SD_AMB_GRP_BEHAVIOR",
        SDLoc(@"响应与省电", @"Timing & Power", @"Отклик и питание"),
        SDLoc(@"延迟防短暂遮挡误切（0 秒立即）；采样间隔越大越省电。手动切换深浅色后将暂停自动，下次解锁或点「立即恢复」后恢复。",
              @"Delay avoids brief shadow triggers (0s = instant); longer intervals save battery. Manual toggles pause auto-switching until next unlock or Resume.",
              @"Задержка исключает ложные срабатывания (0с = сразу); больший интервал экономит заряд. Ручная смена ставит автопереключение на паузу до разблокировки.")));
    add(MakeCompactSliderSpecifier(
        @"SD_AMB_DELAY",
        SDLoc(@"切换延迟", @"Switch Delay", @"Задержка переключения"),
        @SDKEY_DELAY, @"sec", SDMIN_DELAY, SDMAX_DELAY, SDDEF_DELAY, NO));
    add(MakeCompactSliderSpecifier(
        @"SD_AMB_INTERVAL",
        SDLoc(@"采样间隔", @"Sampling Interval", @"Интервал опроса"),
        @SDKEY_INTERVAL, @"sec", SDMIN_INTERVAL, SDMAX_INTERVAL, SDDEF_INTERVAL, NO));
    add(MakeTitleValueSpecifier(
        @"SD_AMB_LP_POLICY",
        SDLoc(@"低电量策略", @"Low Power Policy", @"Стратегия экономии"),
        @selector(getLowPowerPolicyForSpecifier:)));
    if (ReadLowPowerPolicy() == SDLP_SLOW) {
        add(MakeLowPowerIntervalSpecifier());
    }
    add(MakeSwitchSpecifier(
        @"SD_AMB_PAUSE",
        SDLoc(@"手动切换后暂停自动", @"Pause After Manual Switch", @"Пауза после ручной смены"),
        @SDKEY_PAUSE,
        SDDEF_PAUSE));

    return arr;
}

static NSArray *CurrentDisplayedAmbientSpecifiers(id controller)
{
    if (!controller || ![controller respondsToSelector:@selector(specifiers)]) return @[];
    NSArray *all = ((NSArray *(*)(id, SEL))objc_msgSend)(controller, @selector(specifiers));
    NSMutableArray *found = [NSMutableArray array];
    for (id sp in all) {
        if (IsAmbientConfigID(SpecIdent(sp))) [found addObject:sp];
    }
    return found;
}

static void SyncLowPowerIntervalRow(id controller, BOOL show, BOOL animated)
{
    if (!controller) return;
    id existing = SpecForID(controller, @"SD_AMB_LP_INTERVAL");
    if (show == (existing != nil)) return;

    if (show) {
        id slider = MakeLowPowerIntervalSpecifier();
        if (slider &&
            [controller respondsToSelector:@selector(insertContiguousSpecifiers:afterSpecifierID:animated:)] &&
            SpecForID(controller, @"SD_AMB_LP_POLICY")) {
            ((void (*)(id, SEL, id, id, BOOL))objc_msgSend)(
                controller, @selector(insertContiguousSpecifiers:afterSpecifierID:animated:),
                @[slider], @"SD_AMB_LP_POLICY", animated);
        }
    } else if (existing && [controller respondsToSelector:@selector(removeContiguousSpecifiers:animated:)]) {
        ((void (*)(id, SEL, id, BOOL))objc_msgSend)(
            controller, @selector(removeContiguousSpecifiers:animated:), @[existing], animated);
    }
}

static void SetAmbientSpecifiersVisible(id controller, BOOL show, BOOL animated)
{
    NSArray *existing = CurrentDisplayedAmbientSpecifiers(controller);
    BOOL isShown = (existing.count > 0);
    if (show) StartLiveLuxMonitor(controller);
    else StopLiveLuxMonitor();
    if (show == isShown) return;

    if (show) {
        NSArray *specs = BuildAmbientSpecifiers();
        if (!specs.count) return;
        if ([controller respondsToSelector:@selector(insertContiguousSpecifiers:afterSpecifierID:animated:)] &&
            SpecForID(controller, kOurSpecID)) {
            ((void (*)(id, SEL, id, id, BOOL))objc_msgSend)(
                controller, @selector(insertContiguousSpecifiers:afterSpecifierID:animated:),
                specs, kOurSpecID, animated);
        } else if ([controller respondsToSelector:@selector(addSpecifiersFromArray:animated:)]) {
            ((void (*)(id, SEL, id, BOOL))objc_msgSend)(
                controller, @selector(addSpecifiersFromArray:animated:), specs, animated);
        }
        RefreshLiveStatusCellsOnMain();
    } else if ([controller respondsToSelector:@selector(removeContiguousSpecifiers:animated:)]) {
        ((void (*)(id, SEL, id, BOOL))objc_msgSend)(
            controller, @selector(removeContiguousSpecifiers:animated:), existing, animated);
    }
}

static void RebuildAmbientPanelForLanguageChange(id controller)
{
    if (!controller) return;
    id ours = SpecForID(controller, kOurSpecID);
    if (ours && [ours respondsToSelector:@selector(setName:)]) {
        ((void (*)(id, SEL, id))objc_msgSend)(ours, @selector(setName:), SDTitleAmbient());
        ReloadSpecifierAnimated(controller, ours, NO);
    }

    NSArray *existing = CurrentDisplayedAmbientSpecifiers(controller);
    if (existing.count > 0) {
        if ([controller respondsToSelector:@selector(removeContiguousSpecifiers:animated:)]) {
            ((void (*)(id, SEL, id, BOOL))objc_msgSend)(
                controller, @selector(removeContiguousSpecifiers:animated:), existing, NO);
        }
        NSArray *fresh = BuildAmbientSpecifiers();
        if (fresh.count &&
            [controller respondsToSelector:@selector(insertContiguousSpecifiers:afterSpecifierID:animated:)] &&
            SpecForID(controller, kOurSpecID)) {
            ((void (*)(id, SEL, id, id, BOOL))objc_msgSend)(
                controller, @selector(insertContiguousSpecifiers:afterSpecifierID:animated:),
                fresh, kOurSpecID, NO);
        }
        RefreshLiveStatusCellsOnMain();
    }
}

#pragma mark - 点击菜单项处理（立即恢复 / 低电量策略）

static void HandleAmbientRowTap(id controller, UITableView *tv, NSIndexPath *ip, NSString *ident)
{
    if (![controller isKindOfClass:[UIViewController class]]) return;
    UIViewController *vc = (UIViewController *)controller;
    UITableViewCell *cell = (tv && ip) ? [tv cellForRowAtIndexPath:ip] : nil;

    if ([ident isEqualToString:@"SD_AMB_RESUME"]) {
        CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),
                                             CFSTR(SD_NOTE_RESUME),
                                             NULL, NULL, TRUE);
        sPausedByManual = NO;
        RefreshLiveStatusCellsOnMain();
        return;
    }

    if ([ident isEqualToString:@"SD_AMB_LP_POLICY"]) {
        int cur = ReadLowPowerPolicy();
        UIAlertController *sheet = [UIAlertController
            alertControllerWithTitle:SDLoc(@"低电量策略", @"Low Power Policy", @"Стратегия экономии")
                             message:nil
                      preferredStyle:UIAlertControllerStyleActionSheet];

        int policies[3] = { SDLP_KEEP, SDLP_SLOW, SDLP_PAUSE };
        for (int i = 0; i < 3; i++) {
            int p = policies[i];
            NSString *label = LowPowerPolicyTitle(p);
            NSString *title = (cur == p) ? [NSString stringWithFormat:@"✓ %@", label] : label;
            [sheet addAction:[UIAlertAction actionWithTitle:title
                                                      style:UIAlertActionStyleDefault
                                                    handler:^(__unused UIAlertAction *action) {
                if (cur == p) return;
                WriteKey(@SDKEY_LP_POLICY, @(p));
                PostPrefsChanged();
                RefreshDetailTextForSpecID(controller, @"SD_AMB_LP_POLICY", LowPowerPolicyTitle(p));
                SyncLowPowerIntervalRow(controller, p == SDLP_SLOW, YES);
                RefreshLiveStatusCellsOnMain();
            }]];
        }
        [sheet addAction:[UIAlertAction actionWithTitle:SDLoc(@"取消", @"Cancel", @"Отмена")
                                                  style:UIAlertActionStyleCancel
                                                handler:nil]];
        UIPopoverPresentationController *pop = sheet.popoverPresentationController;
        if (pop) {
            pop.sourceView = cell ?: vc.view;
            pop.sourceRect = cell ? cell.bounds : CGRectMake(CGRectGetMidX(vc.view.bounds), CGRectGetMidY(vc.view.bounds), 1, 1);
        }
        [vc presentViewController:sheet animated:YES completion:nil];
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
