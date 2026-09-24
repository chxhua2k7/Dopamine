// BootLogStop:告訴 Dopamine 的 verbose boot log(BaseBin/launchdhook/src/bootlog.c)什麼時候停,
// 讓 log 在蘋果畫面一路捲、鎖定畫面一出現就結束。
//
// 只有 SpringBoard 自己知道鎖定畫面什麼時候要上螢幕,所以由它寫 stop marker;launchd 在每次
// swap 前檢查 marker(另外每 25 ms 輪詢),看到就停,不再多畫一幀。launch daemon 做不到:
// 實機上它要到第一次解鎖後才真的開始跑,早就過了。
//
// launchd 開始畫 log 時會留下 active marker;沒有它(一般開機、respring、Dopamine 關掉
// Verbose Boot)這個插件什麼都不做,連 hook 都不裝。
//
// 停止時機(設定頁的 stopAt,下次 userspace reboot 生效):
//   firstFrame(預設,實機確認剛好):applicationDidFinishLaunching 之後第一次 runloop
//     BeforeWaiting、Core Animation commit 之前(observer order 1999999)。這就是 SpringBoard
//     把鎖定畫面第一幀交給 backboardd 的前一刻;launchd 幾 ms 內停,畫面再過一兩幀才上螢幕。
//   willAppear:CSCoverSheetViewController viewWillAppear:。發生在 applicationDidFinishLaunching
//     還在跑的時候,比畫面真正出現早約 3.5 秒(log 會定格)。
//   didAppear:CSCoverSheetViewController viewDidAppear:。
//
// 每個事件用和 kernel log 同一個時鐘寫進 bootlog_springboard.txt(設定頁的 trace),
// 可以直接跟 bootlog.txt 對時。

#import <UIKit/UIKit.h>
#import <mach/mach_time.h>
#import <sys/stat.h>

#ifndef BLS_VERSION
#error BLS_VERSION comes from PACKAGE_VERSION in the Makefile
#endif

#pragma mark - 路徑(和 BaseBin/launchdhook/src/bootlog.c 保持一致)

#define BLS_DIR "/var/mobile/Library/Logs/Dopamine"
#define BLS_ACTIVE_MARKER_PATH BLS_DIR "/.bootlog_active"
#define BLS_STOP_MARKER_PATH BLS_DIR "/.bootlog_stop"
#define BLS_TRACE_PATH BLS_DIR "/bootlog_springboard.txt"
// 比這個舊的 active marker 是之前某次開機留下的
#define BLS_ACTIVE_MARKER_MAX_AGE 180

#pragma mark - 狀態

static NSString *const kBLSPrefsDomain = @"com.c3x14n.bootlogstop";

typedef NS_ENUM(NSInteger, BLSStopAt) {
    BLSStopAtFirstFrame,
    BLSStopAtWillAppear,
    BLSStopAtDidAppear,
};

static BLSStopAt gStopAt = BLSStopAtFirstFrame;
static BOOL gTrace = YES;
static FILE *gTraceFile;
static BOOL gStopSent;

#pragma mark - 設定

static id BLSCopyPref(NSString *key) {
    CFPropertyListRef value = CFPreferencesCopyAppValue((__bridge CFStringRef)key, (__bridge CFStringRef)kBLSPrefsDomain);
    return CFBridgingRelease(value);
}

// 開機時(可能還沒第一次解鎖)讀不到就用預設值;trace 裡會記下實際用了什麼。
static void BLSLoadPrefs(void) {
    NSString *stopAt = BLSCopyPref(@"stopAt");
    if ([stopAt isEqual:@"willAppear"]) gStopAt = BLSStopAtWillAppear;
    else if ([stopAt isEqual:@"didAppear"]) gStopAt = BLSStopAtDidAppear;
    else gStopAt = BLSStopAtFirstFrame;

    NSNumber *trace = BLSCopyPref(@"trace");
    gTrace = [trace isKindOfClass:[NSNumber class]] ? trace.boolValue : YES;
}

static const char *BLSStopAtName(BLSStopAt stopAt) {
    switch (stopAt) {
        case BLSStopAtWillAppear: return "willAppear";
        case BLSStopAtDidAppear: return "didAppear";
        default: return "firstFrame";
    }
}

#pragma mark - 時間線

// 和 bootlog.txt 的 kernel 行同一個時鐘和格式
static void BLSTrace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void BLSTrace(const char *fmt, ...) {
    if (!gTraceFile) return;
    static mach_timebase_info_data_t timebase;
    if (timebase.denom == 0) mach_timebase_info(&timebase);
    uint64_t ns = mach_continuous_time() * timebase.numer / timebase.denom;
    fprintf(gTraceFile, "[%5llu.%06llu] ", ns / NSEC_PER_SEC, (ns % NSEC_PER_SEC) / NSEC_PER_USEC);
    va_list va;
    va_start(va, fmt);
    vfprintf(gTraceFile, fmt, va);
    va_end(va);
    fputc('\n', gTraceFile);
    fflush(gTraceFile);
}

#pragma mark - 停止訊號

// 事件發生時呼叫(主執行緒);是設定的那個時機才寫 stop marker,每次開機只寫一次。
static void BLSEvent(BLSStopAt event, const char *description) {
    BLSTrace("%s", description);
    if (event != gStopAt || gStopSent) return;
    gStopSent = YES;

    // 先寫暫存檔再 rename,launchd 不會讀到寫一半的內容
    char line[256];
    snprintf(line, sizeof(line), "SpringBoard: %s\n", description);
    const char *tmpPath = BLS_STOP_MARKER_PATH ".tmp";
    int fd = open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        BLSTrace("can't create %s (errno %d: %s)", tmpPath, errno, strerror(errno));
        return;
    }
    write(fd, line, strlen(line));
    close(fd);
    if (rename(tmpPath, BLS_STOP_MARKER_PATH) != 0) {
        BLSTrace("can't rename the stop marker into place (errno %d: %s)", errno, strerror(errno));
        return;
    }
    BLSTrace("stop marker written");
}

#pragma mark - Hooks

%hook SpringBoard

- (void)applicationDidFinishLaunching:(id)application {
    BLSTrace("applicationDidFinishLaunching: begin");
    %orig;
    BLSTrace("applicationDidFinishLaunching: end");

    // Core Animation 在 kCFRunLoopBeforeWaiting、order 2000000 commit;排在它前面的 observer
    // 剛好在 SpringBoard 把啟動後第一幀交給 backboardd 之前執行。只跑一次。
    CFRunLoopObserverRef observer = CFRunLoopObserverCreateWithHandler(kCFAllocatorDefault, kCFRunLoopBeforeWaiting, false, 1999999, ^(CFRunLoopObserverRef o, CFRunLoopActivity activity) {
        BLSEvent(BLSStopAtFirstFrame, "about to commit the first frame after applicationDidFinishLaunching");
    });
    CFRunLoopAddObserver(CFRunLoopGetMain(), observer, kCFRunLoopCommonModes);
    CFRelease(observer);
}

%end

%hook CSCoverSheetViewController

- (void)viewWillAppear:(BOOL)animated {
    BLSEvent(BLSStopAtWillAppear, "lock screen viewWillAppear");
    %orig;
}

- (void)viewDidAppear:(BOOL)animated {
    %orig;
    BLSEvent(BLSStopAtDidAppear, "lock screen viewDidAppear");
}

%end

%ctor {
    @autoreleasepool {
        struct stat st;
        if (stat(BLS_ACTIVE_MARKER_PATH, &st) != 0) return;
        if (time(NULL) - st.st_mtime > BLS_ACTIVE_MARKER_MAX_AGE) return;

        BLSLoadPrefs();
        if (gTrace) gTraceFile = fopen(BLS_TRACE_PATH, "w");
        BLSTrace("BootLogStop %s loaded into SpringBoard (pid %d), boot log active, stop at %s",
                 BLS_VERSION, getpid(), BLSStopAtName(gStopAt));
        %init;
    }
}
