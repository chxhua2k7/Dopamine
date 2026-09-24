//
//  BLSRootListController.m
//  設定頁的 controller。libprefs 的 PLLocalizedListController 負責從 PreferenceLoader
//  entry 旁邊的 plist(prefs/BootLogStop.plist)載入項目並翻譯;這個子類加上:
//  - 列的 SF Symbol 圖示(和 NowLyrics / IslandSwipe 同一套)
//  - 「上次開機」三列:從 launchdhook 寫的 bootlog.txt 讀出時間、log 跑了多久、被什麼停下來
//  - 右上角「套用」按鈕:確認後透過 FrontBoardServices 的 SBSRelaunchAction 重啟 SpringBoard
//

#import <UIKit/UIKit.h>
#import <Preferences/PSListController.h>
#import <libprefs/prefs.h>
#import <dlfcn.h>
#import <objc/runtime.h>
#import <objc/message.h>

#import <Preferences/PSSpecifier.h>

#ifndef BLS_VERSION
#error BLS_VERSION comes from PACKAGE_VERSION in the Makefile
#endif

// launchdhook 停止時寫的完整 log(BaseBin/launchdhook/src/bootlog.c 的 BOOTLOG_LOGFILE_PATH)
static NSString *const kBLSBootLogPath = @"/var/mobile/Library/Logs/Dopamine/bootlog.txt";

@interface BLSRootListController : PLLocalizedListController
@end

#pragma mark - Icons(和 NowLyrics 同一套:列上寫 iconSymbol / iconColor,執行期畫成 SF Symbol 圖示)

static const CGFloat kBLSIconSize = 29;
static const CGFloat kBLSIconCornerRadius = 6.5;
static const CGFloat kBLSIconGlyphPointSize = 15;

static UIColor *BLSColorFromHex(id value) {
    if (![value isKindOfClass:[NSString class]]) return nil;
    NSString *digits = [value hasPrefix:@"#"] ? [value substringFromIndex:1] : value;
    unsigned rgb = 0;
    if (digits.length != 6 || ![[NSScanner scannerWithString:digits] scanHexInt:&rgb]) return nil;
    return [UIColor colorWithRed:((rgb >> 16) & 0xFF) / 255.0 green:((rgb >> 8) & 0xFF) / 255.0 blue:(rgb & 0xFF) / 255.0 alpha:1];
}

static UIImage *BLSTileIcon(NSString *symbolName, UIColor *color) {
    UIImageSymbolConfiguration *configuration = [UIImageSymbolConfiguration configurationWithPointSize:kBLSIconGlyphPointSize weight:UIImageSymbolWeightMedium];
    UIImage *glyph = [[UIImage systemImageNamed:symbolName withConfiguration:configuration]
                      imageWithTintColor:UIColor.whiteColor renderingMode:UIImageRenderingModeAlwaysOriginal];
    CGRect tile = CGRectMake(0, 0, kBLSIconSize, kBLSIconSize);
    UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:tile.size];
    return [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
        [color setFill];
        [[UIBezierPath bezierPathWithRoundedRect:tile cornerRadius:kBLSIconCornerRadius] fill];
        if (!glyph) return;
        CGSize size = glyph.size;
        [glyph drawInRect:CGRectMake((kBLSIconSize - size.width) / 2, (kBLSIconSize - size.height) / 2, size.width, size.height)];
    }];
}

static void BLSApplyIcon(PSSpecifier *specifier) {
    if ([specifier propertyForKey:PSIconImageKey]) return;
    // `icon`:PreferenceLoader 只會替清單入口讀這個 key;頁內的列要自己從 preference bundle 載入。
    NSString *file = [specifier propertyForKey:@"icon"];
    if ([file isKindOfClass:[NSString class]]) {
        UIImage *image = [UIImage imageNamed:file.stringByDeletingPathExtension
                                    inBundle:[NSBundle bundleForClass:BLSRootListController.class]
               compatibleWithTraitCollection:nil];
        if (image) { [specifier setProperty:image forKey:PSIconImageKey]; return; }
    }
    NSString *symbol = [specifier propertyForKey:@"iconSymbol"];
    UIColor *color = BLSColorFromHex([specifier propertyForKey:@"iconColor"]);
    if (![symbol isKindOfClass:[NSString class]] || !color) return;
    [specifier setProperty:BLSTileIcon(symbol, color) forKey:PSIconImageKey];
}

// 字串放在 PreferenceLoader 的 prefs 資料夾(和 plist 同一份 Localizable.strings)。
static NSString *BLSLocalized(NSString *key) {
    static NSBundle *bundle;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        for (NSString *root in @[@"/var/jb", @""]) {
            NSBundle *candidate = [NSBundle bundleWithPath:[root stringByAppendingString:@"/Library/PreferenceLoader/Preferences/BootLogStop"]];
            if (candidate) { bundle = candidate; break; }
        }
    });
    return [bundle localizedStringForKey:key value:key table:nil] ?: key;
}

#pragma mark - 上次開機(解析 bootlog.txt)

typedef struct {
    BOOL found;
    NSDate *date;
    double duration;     // re-exec 到停止的秒數,< 0 表示不知道
    NSString *stoppedBy; // 已翻譯
} BLSLastBoot;

static double BLSTimestamp(NSString *log, NSString *pattern, NSString **captureOut) {
    NSRegularExpression *regex = [NSRegularExpression regularExpressionWithPattern:pattern options:NSRegularExpressionAnchorsMatchLines error:nil];
    NSTextCheckingResult *match = [regex firstMatchInString:log options:0 range:NSMakeRange(0, log.length)];
    if (!match) return -1;
    if (captureOut && match.numberOfRanges > 2) *captureOut = [log substringWithRange:[match rangeAtIndex:2]];
    return [log substringWithRange:[match rangeAtIndex:1]].doubleValue;
}

// launchdhook 的停止原因 → 設定頁上的說法
static NSString *BLSDescribeStopReason(NSString *reason) {
    if (!reason) return BLSLocalized(@"Unknown");
    if ([reason containsString:@"first frame"]) return BLSLocalized(@"First frame");
    if ([reason containsString:@"viewWillAppear"]) return BLSLocalized(@"Lock screen will appear");
    if ([reason containsString:@"viewDidAppear"]) return BLSLocalized(@"Lock screen appeared");
    if ([reason containsString:@"hard cap"]) return BLSLocalized(@"Hard cap (20 s)");
    if ([reason containsString:@"watchdog"]) return BLSLocalized(@"Watchdog (120 s)");
    if ([reason containsString:@"backboardd"]) return BLSLocalized(@"backboardd started");
    return BLSLocalized(@"Unknown");
}

static BLSLastBoot BLSReadLastBoot(void) {
    BLSLastBoot result = { NO, nil, -1, nil };
    NSData *data = [NSData dataWithContentsOfFile:kBLSBootLogPath];
    if (!data) return result;
    // 內容大多是 kernel 訊息,偶爾有非 UTF-8 的位元組
    NSString *log = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]
                 ?: [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding];
    if (!log) return result;

    result.found = YES;
    result.date = [[NSFileManager defaultManager] attributesOfItemAtPath:kBLSBootLogPath error:nil].fileModificationDate;
    double reexec = BLSTimestamp(log, @"^\\[ *([0-9]+\\.[0-9]+)\\] launchd\\[1\\]: userspace reboot: launchd re-executed", NULL);
    NSString *reason = nil;
    double stopped = BLSTimestamp(log, @"^\\[ *([0-9]+\\.[0-9]+)\\] launchd\\[1\\]: boot log stopped \\((.*)\\)$", &reason);
    if (reexec >= 0 && stopped >= reexec) result.duration = stopped - reexec;
    result.stoppedBy = BLSDescribeStopReason(reason);
    return result;
}

@implementation BLSRootListController {
    BLSLastBoot _lastBoot;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:BLSLocalized(@"Apply")
                                                                              style:UIBarButtonItemStyleDone
                                                                             target:self
                                                                             action:@selector(blsApplyTapped)];
}

- (NSMutableArray *)specifiers {
    NSMutableArray *specifiers = [super specifiers];
    for (PSSpecifier *specifier in specifiers) BLSApplyIcon(specifier);
    return specifiers;
}

// 每次進頁面都重新讀一次,重開機後回來就是新的結果
- (void)viewWillAppear:(BOOL)animated {
    _lastBoot = BLSReadLastBoot();
    [super viewWillAppear:animated];
    [self reloadSpecifiers];
}

#pragma mark 「上次開機」三列(plist 的 get)

- (NSString *)blsLastBootDate:(PSSpecifier *)specifier {
    if (!_lastBoot.found || !_lastBoot.date) return BLSLocalized(@"No record yet");
    return [NSDateFormatter localizedStringFromDate:_lastBoot.date dateStyle:NSDateFormatterShortStyle timeStyle:NSDateFormatterShortStyle];
}

- (NSString *)blsLastBootDuration:(PSSpecifier *)specifier {
    if (!_lastBoot.found || _lastBoot.duration < 0) return @"—";
    return [NSString stringWithFormat:BLSLocalized(@"%.1f s"), _lastBoot.duration];
}

- (NSString *)blsLastBootStoppedBy:(PSSpecifier *)specifier {
    if (!_lastBoot.found) return @"—";
    return _lastBoot.stoppedBy;
}

#pragma mark 「關於」

// 版本列(plist 的 get = blsVersion:)。
- (NSString *)blsVersion:(PSSpecifier *)specifier {
    return @BLS_VERSION;
}

#pragma mark 套用

- (void)blsApplyTapped {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:BLSLocalized(@"Apply")
                                                                   message:BLSLocalized(@"Restart SpringBoard to apply?")
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:BLSLocalized(@"Cancel") style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:BLSLocalized(@"Respring") style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
        [self blsRespring];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)blsRespring {
    dlopen("/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices", RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_NOW);
    Class actionClass = objc_getClass("SBSRelaunchAction");
    Class serviceClass = objc_getClass("FBSSystemService");
    if (!actionClass || !serviceClass) return;
    // SBSRelaunchActionOptionsRestartRenderServer = 1 << 0
    id action = ((id (*)(id, SEL, id, NSUInteger, id))objc_msgSend)(actionClass, @selector(actionWithReason:options:targetURL:), @"BootLogStop", 1, nil);
    id service = ((id (*)(id, SEL))objc_msgSend)(serviceClass, @selector(sharedService));
    ((void (*)(id, SEL, id, id))objc_msgSend)(service, @selector(sendActions:withResult:), [NSSet setWithObject:action], nil);
}

@end
