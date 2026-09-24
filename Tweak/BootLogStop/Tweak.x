// BootLogStop: tells Dopamine's verbose boot log (launchdhook/src/bootlog.c)
// when to stop drawing, from the one process that knows when the lock screen
// is about to appear, SpringBoard.
//
// launchd leaves BOOTLOG_ACTIVE_MARKER_PATH behind while the log is running.
// Without it (every normal SpringBoard launch, respring, verbose boot off)
// this tweak does nothing at all. With it, the first of these drops
// BOOTLOG_STOP_MARKER_PATH, which launchd polls for every 25 ms:
//  - the lock screen (CSCoverSheetViewController) is about to appear
//  - SpringBoard commits its first frame after applicationDidFinishLaunching
// Every event is also written to BOOTLOG_TRACE_PATH with the same clock and
// format as the kernel lines in bootlog.txt, so the two can be lined up.

#import <UIKit/UIKit.h>
#import <mach/mach_time.h>
#import <sys/stat.h>

// Keep in sync with BaseBin/launchdhook/src/bootlog.c
#define BOOTLOG_DIR "/var/mobile/Library/Logs/Dopamine"
#define BOOTLOG_ACTIVE_MARKER_PATH BOOTLOG_DIR "/.bootlog_active"
#define BOOTLOG_STOP_MARKER_PATH BOOTLOG_DIR "/.bootlog_stop"
#define BOOTLOG_TRACE_PATH BOOTLOG_DIR "/bootlog_springboard.txt"
// An active marker older than this was left behind by an earlier boot
#define BOOTLOG_ACTIVE_MARKER_MAX_AGE 180

static FILE *gTrace;
static bool gStopSent;

static void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void trace(const char *fmt, ...)
{
	if (!gTrace) return;
	static mach_timebase_info_data_t timebase;
	if (timebase.denom == 0) mach_timebase_info(&timebase);
	uint64_t ns = mach_continuous_time() * timebase.numer / timebase.denom;
	fprintf(gTrace, "[%5llu.%06llu] ", ns / NSEC_PER_SEC, (ns % NSEC_PER_SEC) / NSEC_PER_USEC);
	va_list va;
	va_start(va, fmt);
	vfprintf(gTrace, fmt, va);
	va_end(va);
	fputc('\n', gTrace);
	fflush(gTrace);
}

// Main thread only
static void send_stop(const char *event)
{
	trace("%s", event);
	if (gStopSent) return;
	gStopSent = true;

	char line[256];
	snprintf(line, sizeof(line), "SpringBoard: %s\n", event);
	const char *tmpPath = BOOTLOG_STOP_MARKER_PATH ".tmp";
	int fd = open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		trace("can't create %s (errno %d: %s)", tmpPath, errno, strerror(errno));
		return;
	}
	write(fd, line, strlen(line));
	close(fd);
	if (rename(tmpPath, BOOTLOG_STOP_MARKER_PATH) != 0) {
		trace("can't rename the stop marker into place (errno %d: %s)", errno, strerror(errno));
		return;
	}
	trace("stop marker written");
}

%hook SpringBoard

- (void)applicationDidFinishLaunching:(id)application
{
	trace("applicationDidFinishLaunching: begin");
	%orig;
	trace("applicationDidFinishLaunching: end");

	// Core Animation commits at kCFRunLoopBeforeWaiting with order 2000000, an
	// observer ordered after it sees the first frame SpringBoard hands to
	// backboardd after launching
	CFRunLoopObserverRef observer = CFRunLoopObserverCreateWithHandler(kCFAllocatorDefault, kCFRunLoopBeforeWaiting, false, 2000001, ^(CFRunLoopObserverRef o, CFRunLoopActivity activity) {
		send_stop("first frame committed after applicationDidFinishLaunching");
	});
	CFRunLoopAddObserver(CFRunLoopGetMain(), observer, kCFRunLoopCommonModes);
	CFRelease(observer);
}

%end

%hook CSCoverSheetViewController

- (void)viewWillAppear:(BOOL)animated
{
	send_stop("lock screen viewWillAppear");
	%orig;
}

- (void)viewDidAppear:(BOOL)animated
{
	%orig;
	trace("lock screen viewDidAppear");
}

%end

%ctor
{
	struct stat st;
	if (stat(BOOTLOG_ACTIVE_MARKER_PATH, &st) != 0) return;
	if (time(NULL) - st.st_mtime > BOOTLOG_ACTIVE_MARKER_MAX_AGE) return;

	gTrace = fopen(BOOTLOG_TRACE_PATH, "w");
	trace("BootLogStop loaded into SpringBoard (pid %d), boot log active", getpid());
	%init;
}
