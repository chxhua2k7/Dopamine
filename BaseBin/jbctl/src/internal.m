#import "internal.h"
#import <Foundation/Foundation.h>
#import <libjailbreak/libjailbreak.h>
#import <sys/mount.h>
#import <notify.h>
#import <mach/mach_time.h>
#import <libproc.h>
#import <sys/proc_info.h>
#import <libjailbreak/stock_fixes.h>

// Companion of the verbose boot log in launchdhook (bootlog.c), paths must
// match. Not in /private/var/tmp: dirs_cleaner empties it during boot.
#define BOOTLOG_ACTIVE_MARKER_PATH "/var/mobile/Library/Logs/Dopamine/.bootlog_active"
#define BOOTLOG_STOP_MARKER_PATH "/var/mobile/Library/Logs/Dopamine/.bootlog_stop"
// What the daemon did on the last boot, overwritten every time it runs
#define BOOTLOG_WATCH_TRACE_PATH "/var/mobile/Library/Logs/Dopamine/bootlog_watch.txt"
#define BOOTLOG_WATCH_TIMEOUT_SECONDS 60
// An active marker older than this was left behind by an earlier boot
#define BOOTLOG_ACTIVE_MARKER_MAX_AGE 180

// Exit codes, visible as "last exit code" in launchctl print
#define BOOTLOG_WATCH_EXIT_NO_MARKER 10
#define BOOTLOG_WATCH_EXIT_STALE_MARKER 11
#define BOOTLOG_WATCH_EXIT_WRITE_FAILED 12

static FILE *gBootlogTrace;

static double continuous_seconds(uint64_t t)
{
	static mach_timebase_info_data_t timebase;
	if (timebase.denom == 0) mach_timebase_info(&timebase);
	return (double)(t * timebase.numer / timebase.denom) / NSEC_PER_SEC;
}

// Same clock and format as the kernel lines in bootlog.txt, so both line up
static void bootlog_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void bootlog_trace(const char *fmt, ...)
{
	if (!gBootlogTrace) return;
	static mach_timebase_info_data_t timebase;
	if (timebase.denom == 0) mach_timebase_info(&timebase);
	uint64_t ns = mach_continuous_time() * timebase.numer / timebase.denom;
	fprintf(gBootlogTrace, "[%5llu.%06llu] ", ns / NSEC_PER_SEC, (ns % NSEC_PER_SEC) / NSEC_PER_USEC);
	va_list va;
	va_start(va, fmt);
	vfprintf(gBootlogTrace, fmt, va);
	va_end(va);
	fputc('\n', gBootlogTrace);
	fflush(gBootlogTrace);
}

static int bootlog_watch_run(void)
{
	bootlog_trace("bootlog_watch started (pid %d, uid %d, euid %d)", getpid(), getuid(), geteuid());

	struct stat st;
	if (stat(BOOTLOG_ACTIVE_MARKER_PATH, &st) != 0) {
		bootlog_trace("no active marker (errno %d: %s), nothing to do", errno, strerror(errno));
		return BOOTLOG_WATCH_EXIT_NO_MARKER;
	}
	long age = (long)(time(NULL) - st.st_mtime);
	bootlog_trace("active marker found, %ld s old", age);
	if (age > BOOTLOG_ACTIVE_MARKER_MAX_AGE) {
		bootlog_trace("active marker is stale, nothing to do");
		return BOOTLOG_WATCH_EXIT_STALE_MARKER;
	}

	// Serial queue so the callbacks below can't race each other
	dispatch_queue_t queue = dispatch_queue_create("com.opa334.Dopamine.bootlog.watch", DISPATCH_QUEUE_SERIAL);
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	NSMutableString *seen = [NSMutableString new];
	__block NSString *stopReason = nil;
	NSDate *start = [NSDate date];

	// The first two are posted by SpringBoard when it is done launching, the
	// others are only recorded (with their timing) to learn how they relate
	struct { const char *name; bool stops; } notifications[] = {
		{ "SBSpringBoardDidLaunchNotification", true },
		{ "com.apple.springboard.finishedstartup", true },
		{ "com.apple.springboard.lockstate", false },
		{ "com.apple.springboard.lockcomplete", false },
	};
	size_t count = sizeof(notifications) / sizeof(notifications[0]);
	int tokens[sizeof(notifications) / sizeof(notifications[0])];
	for (size_t i = 0; i < count; i++) {
		const char *name = notifications[i].name;
		bool stops = notifications[i].stops;
		tokens[i] = 0;
		int token = 0;
		uint32_t r = notify_register_dispatch(name, &token, queue, ^(int t) {
			bootlog_trace("notification %s", name);
			[seen appendFormat:@"%s@%.3fs ", name, [[NSDate date] timeIntervalSinceDate:start]];
			if (stops && !stopReason) {
				stopReason = [NSString stringWithFormat:@"SpringBoard finished launching (%s)", name];
				dispatch_semaphore_signal(done);
			}
		});
		bootlog_trace("registered %s: status %u", name, r);
		if (r == NOTIFY_STATUS_OK) tokens[i] = token;
	}

	long waitResult = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)BOOTLOG_WATCH_TIMEOUT_SECONDS * (int64_t)NSEC_PER_SEC));
	bootlog_trace("wait finished: %s", waitResult == 0 ? "signalled" : "timed out");
	for (size_t i = 0; i < count; i++) {
		if (tokens[i]) notify_cancel(tokens[i]);
	}

	__block NSString *reason = nil;
	dispatch_sync(queue, ^{
		reason = (waitResult == 0 && stopReason) ? stopReason : @"timed out waiting for SpringBoard";
		if (seen.length) reason = [NSString stringWithFormat:@"%@ [seen: %@]", reason, seen];
	});

	// Write atomically, launchd polls for this file
	const char *tmpPath = BOOTLOG_STOP_MARKER_PATH ".tmp";
	const char *line = [reason stringByAppendingString:@"\n"].UTF8String;
	int fd = open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		bootlog_trace("can't create %s (errno %d: %s)", tmpPath, errno, strerror(errno));
		return BOOTLOG_WATCH_EXIT_WRITE_FAILED;
	}
	write(fd, line, strlen(line));
	close(fd);
	if (rename(tmpPath, BOOTLOG_STOP_MARKER_PATH) != 0) {
		bootlog_trace("can't rename stop marker into place (errno %d: %s)", errno, strerror(errno));
		return BOOTLOG_WATCH_EXIT_WRITE_FAILED;
	}
	bootlog_trace("stop marker written: %s", reason.UTF8String);
	return 0;
}

// launchd cannot use libnotify itself, so this (spawned by the
// com.opa334.Dopamine.bootlog launch daemon on every userspace boot) waits for
// SpringBoard to report that it finished launching and then drops a marker
// file that tells launchdhook to stop drawing the boot log, right before
// SpringBoard puts its first frame on screen. Returns immediately when no
// boot log is active.
static int bootlog_watch(void)
{
	uint64_t beforeMkdir = mach_continuous_time();
	mkdir("/var/mobile/Library/Logs/Dopamine", 0755);
	uint64_t beforeOpen = mach_continuous_time();
	gBootlogTrace = fopen(BOOTLOG_WATCH_TRACE_PATH, "w");
	uint64_t afterOpen = mach_continuous_time();

	// Where the time between launchd's spawn and us getting here went. The
	// process start time is wall clock, turn it into seconds on our clock.
	struct proc_bsdinfo info;
	double started = -1;
	if (proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info)) {
		struct timeval now;
		gettimeofday(&now, NULL);
		double ago = (now.tv_sec - (double)info.pbi_start_tvsec) + (now.tv_usec - (double)info.pbi_start_tvusec) / 1e6;
		started = continuous_seconds(mach_continuous_time()) - ago;
	}
	bootlog_trace("timing: process created %.3f, jbctl constructor %.3f, main %.3f, mkdir %.3f, fopen %.3f -> %.3f",
		started, continuous_seconds(gJbctlConstructorTime), continuous_seconds(gJbctlMainTime),
		continuous_seconds(beforeMkdir), continuous_seconds(beforeOpen), continuous_seconds(afterOpen));

	int r = bootlog_watch_run();
	bootlog_trace("exiting with %d", r);
	if (gBootlogTrace) {
		fclose(gBootlogTrace);
		gBootlogTrace = NULL;
	}
	return r;
}

SInt32 CFUserNotificationDisplayAlert(CFTimeInterval timeout, CFOptionFlags flags, CFURLRef iconURL, CFURLRef soundURL, CFURLRef localizationURL, CFStringRef alertHeader, CFStringRef alertMessage, CFStringRef defaultButtonTitle, CFStringRef alternateButtonTitle, CFStringRef otherButtonTitle, CFOptionFlags *responseFlags) API_AVAILABLE(ios(3.0));

void execute_unsandboxed(void (^block)(void))
{
	uint64_t credBackup = 0;
	jbclient_root_steal_ucred(0, &credBackup);
	block();
	jbclient_root_steal_ucred(credBackup, NULL);
}

int mount_unsandboxed(const char *type, const char *dir, int flags, void *data)
{
	__block int r = 0;
	execute_unsandboxed(^{
		r = mount(type, dir, flags, data);
	});
	return r;
}

int unmount_unsandboxed(const char *dir, int flags)
{
	__block int r = 0;
	execute_unsandboxed(^{
		r = unmount(dir, flags);
	});
	return r;
}

bool is_protected(const char *path)
{
	struct statfs sb;
	statfs(path, &sb);
	return strcmp(path, sb.f_mntonname) == 0;
}

int ensure_protected(const char *path)
{
	if (!is_protected(path)) {
		return mount_unsandboxed("bindfs", path, 0, (void *)path);
	}
	return 0;
}

int ensure_unprotected(const char *path)
{
	if (is_protected(path)) {
		return unmount_unsandboxed(path, MNT_FORCE);
	}
	return 0;
}

int protection_set_active(bool active)
{
	int r = 0;
	if (active) {
		// Protect /private/preboot/UUID/<System, usr> from being modified by bind mounting them on top of themselves
		// This protects dumb users from accidentally deleting these, which would induce a recovery loop after rebooting
		r |= ensure_protected(prebootUUIDPath("/System"));
		r |= ensure_protected(prebootUUIDPath("/usr"));
	}
	else {
		r |= ensure_unprotected(prebootUUIDPath("/System"));
		r |= ensure_unprotected(prebootUUIDPath("/usr"));
	}
	return r;
}

bool fakelib_is_mounted(void)
{
	struct statfs fsb;
    if (statfs("/usr/lib", &fsb) != 0) return NO;
    return strcmp(fsb.f_mntonname, "/usr/lib") == 0;
}

int fakelib_set_mounted(bool mounted)
{
	int r = 0;
	if (mounted != fakelib_is_mounted()) {
		if (mounted) {
			r = mount_unsandboxed("bindfs", "/usr/lib", MNT_RDONLY, (void *)JBROOT_PATH("/basebin/.fakelib"));
		}
		else {
			r = unmount_unsandboxed("/usr/lib", MNT_FORCE);
		}
	}
	return r;
}

int jbctl_handle_internal(const char *command, int argc, char* argv[])
{
	if (!strcmp(command, "launchd_stash_port")) {
		mach_port_t *selfInitPorts = NULL;
		mach_msg_type_number_t selfInitPortsCount = 0;
		if (mach_ports_lookup(mach_task_self(), &selfInitPorts, &selfInitPortsCount) != 0) {
			printf("ERROR: Failed port lookup on self\n");
			return -1;
		}
		if (selfInitPortsCount < 3) {
			printf("ERROR: Unexpected initports count on self\n");
			return -1;
		}
		if (selfInitPorts[2] == MACH_PORT_NULL) {
			printf("ERROR: Port to stash not set\n");
			return -1;
		}

		printf("Port to stash: %u\n", selfInitPorts[2]);

		mach_port_t launchdTaskPort;
		if (task_for_pid(mach_task_self(), 1, &launchdTaskPort) != 0) {
			printf("task_for_pid on launchd failed\n");
			return -1;
		}
		mach_port_t *launchdInitPorts = NULL;
		mach_msg_type_number_t launchdInitPortsCount = 0;
		if (mach_ports_lookup(launchdTaskPort, &launchdInitPorts, &launchdInitPortsCount) != 0) {
			printf("mach_ports_lookup on launchd failed\n");
			return -1;
		}
		if (launchdInitPortsCount < 3) {
			printf("ERROR: Unexpected initports count on launchd\n");
			return -1;
		}
		launchdInitPorts[2] = selfInitPorts[2]; // Transfer port to launchd
		if (mach_ports_register(launchdTaskPort, launchdInitPorts, launchdInitPortsCount) != 0) {
			printf("ERROR: Failed stashing port into launchd\n");
			return -1;
		}
		mach_port_deallocate(mach_task_self(), launchdTaskPort);
		return 0;
	}
	else if (!strcmp(command, "protection")) {
		bool toSet = false;
		if (argc > 1) {
			if (!strcmp(argv[1], "activate")) {
				toSet = true;
			}
			else if (!strcmp(argv[1], "deactivate")) {
				toSet = false;
			}
			else {
				return -1;
			}

			return protection_set_active(toSet);
		}
		return -1;
	}
	else if (!strcmp(command, "fakelib")) {
		bool toMount = false;
		if (argc > 1) {
			if (!strcmp(argv[1], "mount")) {
				toMount = true;
			}
			else if (!strcmp(argv[1], "unmount")) {
				toMount = false;
			}
			else {
				return -1;
			}

			return fakelib_set_mounted(toMount);
		}
		return -1;
	}
	else if (!strcmp(command, "startup")) {
		protection_set_active(true);
		char *panicMessage = NULL;
		if (jbclient_watchdog_get_last_userspace_panic(&panicMessage) == 0) {
			NSString *printMessage = [NSString stringWithFormat:@"Dopamine has protected you from a userspace panic by temporarily disabling tweak injection and triggering a userspace reboot instead. A log is available under Analytics in the Preferences app. You can reenable tweak injection in the Dopamine app.\n\nPanic message: \n%s", panicMessage];
			CFUserNotificationDisplayAlert(0, 2/*kCFUserNotificationCautionAlertLevel*/, NULL, NULL, NULL, CFSTR("Watchdog Timeout"), (__bridge CFStringRef)printMessage, NULL, NULL, NULL, NULL);
			free(panicMessage);
		}
		exec_cmd(JBROOT_PATH("/usr/bin/uicache"), "-a", NULL);
	}
	else if (!strcmp(command, "bootlog_watch")) {
		return bootlog_watch();
	}
	else if (!strcmp(command, "install_pkg")) {
		if (argc > 1) {
			extern char **environ;
			const char *dpkg = JBROOT_PATH("/usr/bin/dpkg");
			int r = execve(dpkg, (char *const *)(const char *[]){dpkg, "-i", argv[1], NULL}, environ);
			return r;
		}
		return -1;
	}
	return -1;
}
