#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <libjailbreak/display.h>
#include <libjailbreak/info.h>
#include "bootlog.h"

struct system_info gSystemInfo = { .jailbreakInfo = { .rootPath = "/var/jb" } };
static int gRotation = 0;
static int gW = 1179, gH = 2556;
static struct drawctx gCtxs[2];
static int gCtxUsed = 0;
static struct drawctx gCtx; // the one currently "on screen"
static uint8_t *gFb;
static int gFlushes = 0;
static int gSwapId = 1000;
static int gForeignSwapsPending = 0; // set by the test to simulate backboardd presenting
static void do_swap(struct drawctx *c) { gSwapId += gForeignSwapsPending; gForeignSwapsPending = 0; c->lastSwapToken = ++gSwapId; }

void exec_with_asl_disabled(void (^block)(void)) { block(); }
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	(void)newp; (void)newlen;
	const char *v = NULL;
	if (!strcmp(name, "kern.version")) v = "Darwin Kernel Version 22.5.0: Mon Apr 24 20:53:19 PDT 2023; root:xnu-8796.122.4~1/RELEASE_ARM64_T8120\n";
	else if (!strcmp(name, "kern.bootargs")) v = "";
	else if (!strcmp(name, "hw.machine")) v = "iPhone15,2";
	else if (!strcmp(name, "hw.model")) v = "D73AP";
	else if (!strcmp(name, "kern.msgbuf")) v = "AppleARMPMUCharger: charging\nAUC: state 4\n\0apfs_vfsop_mount: mounted /private/var\nAppleH13CamIn::ISP_ProcessError: error 0xe00002c2 timed out\nIOKit: some long kernel line that is definitely going to need to wrap around the screen because it is way longer than the number of columns we have available on this device\n";
	if (!v) return -1;
	size_t len = (v == NULL) ? 0 : (strcmp(name, "kern.msgbuf") ? strlen(v) : strlen(v) + 1 + strlen(v + strlen(v) + 1));
	if (oldp) { if (*oldlenp < len) len = *oldlenp; memcpy(oldp, v, len); }
	*oldlenp = len;
	return 0;
}
struct drawctx *drawctx_init(void) {
	if (gCtxUsed >= 2) return NULL;
	struct drawctx *c = &gCtxs[gCtxUsed++];
	c->size.width = (gRotation == 90 || gRotation == 270) ? gH : gW;
	c->size.height = (gRotation == 90 || gRotation == 270) ? gW : gH;
	c->bytesPerRow = ((int)c->size.width * 4 + 63) / 64 * 64;
	c->base = calloc(1, (size_t)c->size.height * c->bytesPerRow); c->inited = true;
	do_swap(c);
	gCtx = *c; gFb = c->base;
	return c;
}
void drawctx_free(struct drawctx *ctx) { ctx->inited = false; gCtxUsed--; }
int drawctx_draw_raw(struct drawctx *ctx, void *rawBuf, size_t rawBufSize) {
	if (!ctx->inited) { fprintf(stderr, "draw into freed ctx\n"); abort(); }
	size_t expect = (size_t)ctx->size.height * ctx->bytesPerRow;
	if (rawBufSize != expect) { fprintf(stderr, "size mismatch %zu vs %zu\n", rawBufSize, expect); abort(); }
	memcpy(ctx->base, rawBuf, rawBufSize); gFlushes++;
	do_swap(ctx);
	gCtx = *ctx; gFb = ctx->base; // this surface is now on screen
	return 0;
}
CGFloat get_main_screen_rotation(void) { return gRotation; }

static void dump_ppm(const char *path) {
	FILE *f = fopen(path, "wb");
	int W = (int)gCtx.size.width, H = (int)gCtx.size.height;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
		uint8_t *p = gFb + (size_t)y * gCtx.bytesPerRow + x * 4; // B G R A
		fputc(p[2], f); fputc(p[1], f); fputc(p[0], f);
	}
	fclose(f);
}

int main(int argc, char **argv) {
	if (argc > 1) gRotation = atoi(argv[1]);
	if (argc > 3) { gW = atoi(argv[2]); gH = atoi(argv[3]); }
	unlink("/var/mobile/Library/Logs/Dopamine/.bootlog_persist"); (void)gCtxs;
	// phase 1
	if (bootlog_start(true) != 0) { fprintf(stderr, "start failed\n"); return 1; }
	bootlog_printf("launchd[1]: re-executing /sbin/launchd");
	bootlog_stop("test");
	// phase 2: restore
	if (bootlog_start(false) != 0) { fprintf(stderr, "start2 failed\n"); return 1; }
	bootlog_dopamine_printf("Dopamine: kernel primitives recovered from boomerang");
	bootlog_dopamine_printf("Dopamine: Failed updating basebin (error 7).");
	char *xargv[] = { "/usr/libexec/xpcproxy", "com.apple.logd\n", NULL };
	bootlog_spawn_event("/usr/libexec/xpcproxy", xargv);
	bootlog_spawn_event("/usr/sbin/notifyd", NULL);
	int n = argc > 4 ? atoi(argv[4]) : 120; for (int i = 0; i < n; i++) {
		char label[64]; snprintf(label, sizeof(label), "com.apple.daemon%d\n", i);
		char *a[] = { "/usr/libexec/xpcproxy", label, NULL };
		bootlog_spawn_event("/usr/libexec/xpcproxy", a);
	}
	usleep(60000);
	// SpringBoard is spawned directly and before backboardd on iOS 16, must NOT stop the log
	bootlog_spawn_event("/System/Library/CoreServices/SpringBoard.app/SpringBoard", NULL);
	if (!bootlog_is_active()) { fprintf(stderr, "STOPPED ON SPRINGBOARD\n"); return 1; }
	usleep(60000);
	char *bb[] = { "/usr/libexec/xpcproxy", "com.apple.backboardd\n", NULL };
	bootlog_spawn_event("/usr/libexec/xpcproxy", bb);
	int watcher = access("/var/jb/Library/MobileSubstrate/DynamicLibraries/BootLogStop.dylib", F_OK) == 0;
	if (watcher) {
		// With the daemon installed the log must keep going after backboardd...
		if (!bootlog_is_active()) { fprintf(stderr, "STOPPED ON BACKBOARDD DESPITE WATCHER\n"); return 1; }
		if (access("/var/mobile/Library/Logs/Dopamine/.bootlog_active", F_OK) != 0) { fprintf(stderr, "NO ACTIVE MARKER\n"); return 1; }
		for (int i = 0; i < 30; i++) {
			char label[64]; snprintf(label, sizeof(label), "com.apple.late%d\n", i);
			char *a[] = { "/usr/libexec/xpcproxy", label, NULL };
			bootlog_spawn_event("/usr/libexec/xpcproxy", a);
			usleep(10000);
		}
		if (!bootlog_is_active()) { fprintf(stderr, "STOPPED TOO EARLY\n"); return 1; }
		if (getenv("FOREIGN")) {
			// backboardd presents SpringBoard's first frame: our next flush must notice and stop
			gForeignSwapsPending = 1;
			usleep(60000);
			bootlog_printf("line after a foreign swap");
			usleep(20000);
			if (!bootlog_is_active()) { fprintf(stderr, "OBSERVE MODE STOPPED THE LOG\n"); return 1; }
			FILE *m2 = fopen("/var/mobile/Library/Logs/Dopamine/.bootlog_stop", "w"); fputs("SpringBoard finished launching (test)\n", m2); fclose(m2);
			usleep(150000);
		}
		else {
			// ...until the daemon drops the marker
			FILE *m = fopen("/var/mobile/Library/Logs/Dopamine/.bootlog_stop", "w"); fputs("SpringBoard finished launching (test)\n", m); fclose(m);
			usleep(150000);
		}
		if (bootlog_is_active()) { fprintf(stderr, "MARKER IGNORED\n"); return 1; }
		if (access("/var/mobile/Library/Logs/Dopamine/.bootlog_active", F_OK) == 0) { fprintf(stderr, "ACTIVE MARKER LEFT BEHIND\n"); return 1; }
	}
	else {
		if (bootlog_is_active()) { fprintf(stderr, "DID NOT STOP ON BACKBOARDD WITHOUT WATCHER\n"); return 1; }
	}
	int flushesAtStop = gFlushes;
	char *later[] = { "/usr/libexec/xpcproxy", "com.apple.foo\n", NULL };
	bootlog_spawn_event("/usr/libexec/xpcproxy", later);
	bootlog_printf("must not show");
	usleep(100000);
	if (gFlushes != flushesAtStop) { fprintf(stderr, "FLUSHED AFTER STOP\n"); return 1; }
	char out[64]; snprintf(out, sizeof(out), "out_%d.ppm", gRotation);
	dump_ppm(out);
	printf("ok rotation=%d flushes=%d\n", gRotation, gFlushes);
	return 0;
}
