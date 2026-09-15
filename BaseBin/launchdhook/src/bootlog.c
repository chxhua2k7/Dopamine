#include "bootlog.h"
#include "bootlog_font.h"

#include <libjailbreak/display.h>
#include <libjailbreak/info.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

// main.m
extern void exec_with_asl_disabled(void (^block)(void));

// proc_kmsgbuf (libproc, part of libSystem) is what dmesg(8) uses. It is not
// declared in every SDK's libproc.h, so it is resolved at runtime; a missing
// symbol must never take launchd down with it.
typedef int (*proc_kmsgbuf_t)(void *buffer, uint32_t buffersize);
static proc_kmsgbuf_t resolve_proc_kmsgbuf(void)
{
	static proc_kmsgbuf_t fn = NULL;
	static bool resolved = false;
	if (!resolved) {
		fn = (proc_kmsgbuf_t)dlsym(RTLD_DEFAULT, "proc_kmsgbuf");
		resolved = true;
	}
	return fn;
}

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

// Maximum characters kept per visual row (lines longer than the screen wrap)
#define BOOTLOG_MAX_COLS 512

// Where the log is stashed across the launchd re-exec
#define BOOTLOG_PERSIST_PATH "/private/var/tmp/.dopamine_bootlog"
// Ignore a stashed log older than this (seconds), it is from another boot
#define BOOTLOG_PERSIST_MAX_AGE 180

// Size of the buffer used to read the kernel message buffer
#define BOOTLOG_KMSG_BUFSIZE (256 * 1024)
// How many dmesg lines to show initially
#define BOOTLOG_KMSG_INITIAL_LINES 24
// Minimum interval between two dmesg polls (nanoseconds)
#define BOOTLOG_KMSG_POLL_INTERVAL_NS (150ull * 1000 * 1000)

// Framebuffer pixel format is 32 bit BGRA little endian -> 0xAARRGGBB as uint32
#define BOOTLOG_COLOR_BG      0xFF000000u
#define BOOTLOG_COLOR_TEXT    0xFFFFFFFFu
#define BOOTLOG_COLOR_KERNEL  0xFFB4B4B4u
#define BOOTLOG_COLOR_HEADER  0xFF9CC4FFu

// Row types (also used as the first byte of each persisted line)
#define BOOTLOG_ROW_TEXT   'T'
#define BOOTLOG_ROW_KERNEL 'K'
#define BOOTLOG_ROW_HEADER 'H'
// Persisted file only: the last kernel message that was already shown
#define BOOTLOG_PERSIST_KMSG_ANCHOR 'A'

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct bootlog_row {
	char type;
	char text[BOOTLOG_MAX_COLS + 1];
};

static struct {
	bool active;
	bool persist;

	struct drawctx *ctx;
	uint32_t *shadow;     // CPU side copy of the framebuffer, copied over on flush
	size_t shadowSize;
	int fbWidth;
	int fbHeight;
	int fbStride;         // in pixels

	int rotation;         // 0 / 90 / 180 / 270, see get_main_screen_rotation
	int logicalWidth;     // size of the screen as the user sees it
	int logicalHeight;

	int scale;            // font scale factor
	int cellWidth;        // pixel size of one character cell
	int cellHeight;
	int cols;
	int rows;
	int originX;          // pixel origin of the text grid (logical coordinates)
	int originY;

	struct bootlog_row *ring;
	int ringHead;         // index of the oldest row
	int ringCount;

	char *kmsgBuf;
	char kmsgLastLine[256];
	uint64_t kmsgLastPoll;
} g;

static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint64_t monotonic_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// Seconds since boot, like the timestamps in the real verbose boot
static double uptime_seconds(void)
{
	return (double)monotonic_ns() / 1e9;
}

static uint32_t color_for_type(char type)
{
	switch (type) {
		case BOOTLOG_ROW_KERNEL: return BOOTLOG_COLOR_KERNEL;
		case BOOTLOG_ROW_HEADER: return BOOTLOG_COLOR_HEADER;
		default: return BOOTLOG_COLOR_TEXT;
	}
}

static int sysctl_string(const char *name, char *out, size_t outSize)
{
	size_t len = outSize - 1;
	if (sysctlbyname(name, out, &len, NULL, 0) != 0) {
		out[0] = '\0';
		return -1;
	}
	out[len] = '\0';
	// These strings sometimes end in a newline
	while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
		out[--len] = '\0';
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Pixel plumbing
// ---------------------------------------------------------------------------

// Map a logical (upright) pixel to the physical framebuffer.
// The mapping is identical to what draw_image_to_buf does with an upright
// image, so rotated iPads come out the right way round.
static inline void put_pixel(int lx, int ly, uint32_t color)
{
	int px, py;
	switch (g.rotation) {
		case 90:
			px = ly;
			py = g.fbHeight - 1 - lx;
			break;
		case 180:
			px = g.fbWidth - 1 - lx;
			py = g.fbHeight - 1 - ly;
			break;
		case 270:
			px = g.fbWidth - 1 - ly;
			py = lx;
			break;
		default:
			px = lx;
			py = ly;
			break;
	}
	if (px < 0 || py < 0 || px >= g.fbWidth || py >= g.fbHeight) return;
	g.shadow[(size_t)py * g.fbStride + px] = color;
}

static void draw_cell(int col, int row, char ch, uint32_t fg)
{
	int glyphIndex = 0; // space
	if (ch >= BOOTLOG_FONT_FIRST_CHAR && ch <= BOOTLOG_FONT_LAST_CHAR) {
		glyphIndex = ch - BOOTLOG_FONT_FIRST_CHAR;
	}
	const uint8_t *glyph = gBootlogFont[glyphIndex];

	int x0 = g.originX + col * g.cellWidth;
	int y0 = g.originY + row * g.cellHeight;
	int scale = g.scale;

	if (g.rotation == 0) {
		// Fast path: rows are contiguous in memory
		for (int r = 0; r < BOOTLOG_FONT_HEIGHT; r++) {
			uint8_t bits = glyph[r];
			for (int sy = 0; sy < scale; sy++) {
				int py = y0 + r * scale + sy;
				if (py < 0 || py >= g.fbHeight) continue;
				uint32_t *line = &g.shadow[(size_t)py * g.fbStride];
				for (int b = 0; b < BOOTLOG_FONT_WIDTH; b++) {
					uint32_t color = (bits & (0x80 >> b)) ? fg : BOOTLOG_COLOR_BG;
					int px = x0 + b * scale;
					for (int sx = 0; sx < scale; sx++, px++) {
						if (px >= 0 && px < g.fbWidth) line[px] = color;
					}
				}
			}
		}
		return;
	}

	for (int r = 0; r < BOOTLOG_FONT_HEIGHT; r++) {
		uint8_t bits = glyph[r];
		for (int b = 0; b < BOOTLOG_FONT_WIDTH; b++) {
			uint32_t color = (bits & (0x80 >> b)) ? fg : BOOTLOG_COLOR_BG;
			for (int sy = 0; sy < scale; sy++) {
				for (int sx = 0; sx < scale; sx++) {
					put_pixel(x0 + b * scale + sx, y0 + r * scale + sy, color);
				}
			}
		}
	}
}

// Render visual row `row` (0 = top of screen) from the ring buffer
static void render_row(int row)
{
	const char *text = "";
	uint32_t fg = BOOTLOG_COLOR_TEXT;
	if (row < g.ringCount) {
		struct bootlog_row *r = &g.ring[(g.ringHead + row) % g.rows];
		text = r->text;
		fg = color_for_type(r->type);
	}

	size_t len = strlen(text);
	for (int col = 0; col < g.cols; col++) {
		char ch = (col < (int)len) ? text[col] : ' ';
		draw_cell(col, row, ch, fg);
	}
}

static void render_all(void)
{
	for (int row = 0; row < g.rows; row++) {
		render_row(row);
	}
}

static void flush(void)
{
	if (!g.ctx || !g.shadow) return;
	exec_with_asl_disabled(^{
		drawctx_draw_raw(g.ctx, g.shadow, g.shadowSize);
	});
}

// ---------------------------------------------------------------------------
// Text buffer
// ---------------------------------------------------------------------------

// Append one visual row (no wrapping, no timestamp). Returns true if the
// screen scrolled, in which case everything needs to be re-rendered.
static bool push_row(char type, const char *text, size_t len)
{
	bool scrolled = false;
	int index;

	if (g.ringCount < g.rows) {
		index = (g.ringHead + g.ringCount) % g.rows;
		g.ringCount++;
	}
	else {
		g.ringHead = (g.ringHead + 1) % g.rows;
		index = (g.ringHead + g.rows - 1) % g.rows;
		scrolled = true;
	}

	struct bootlog_row *row = &g.ring[index];
	row->type = type;
	if (len > BOOTLOG_MAX_COLS) len = BOOTLOG_MAX_COLS;
	memcpy(row->text, text, len);
	row->text[len] = '\0';

	if (!scrolled) {
		render_row(g.ringCount - 1);
	}
	return scrolled;
}

// Append a full line: sanitizes it, wraps it to the screen width and renders
// the affected rows into the shadow buffer (does not flush).
static void append_line_locked(char type, const char *line)
{
	char clean[BOOTLOG_MAX_COLS + 1];
	size_t n = 0;
	for (const char *p = line; *p && n < BOOTLOG_MAX_COLS; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == '\t') {
			// Expand tabs to 4 spaces
			for (int i = 0; i < 4 && n < BOOTLOG_MAX_COLS; i++) clean[n++] = ' ';
		}
		else if (c == '\n' || c == '\r') {
			break;
		}
		else if (c < 0x20 || c > 0x7E) {
			clean[n++] = '?';
		}
		else {
			clean[n++] = (char)c;
		}
	}
	// Trim trailing whitespace
	while (n > 0 && clean[n - 1] == ' ') n--;
	clean[n] = '\0';

	bool scrolled = false;
	if (n == 0) {
		scrolled |= push_row(type, "", 0);
	}
	else {
		size_t offset = 0;
		while (offset < n) {
			size_t chunk = n - offset;
			if (chunk > (size_t)g.cols) chunk = (size_t)g.cols;
			scrolled |= push_row(type, clean + offset, chunk);
			offset += chunk;
		}
	}

	if (scrolled) {
		render_all();
	}
}

static void persist_locked(void)
{
	if (!g.persist) return;

	char tmpPath[] = BOOTLOG_PERSIST_PATH ".tmp";
	FILE *f = fopen(tmpPath, "w");
	if (!f) return;
	if (g.kmsgLastLine[0]) {
		fputc(BOOTLOG_PERSIST_KMSG_ANCHOR, f);
		fputs(g.kmsgLastLine, f);
		fputc('\n', f);
	}
	for (int i = 0; i < g.ringCount; i++) {
		struct bootlog_row *row = &g.ring[(g.ringHead + i) % g.rows];
		fputc(row->type, f);
		fputs(row->text, f);
		fputc('\n', f);
	}
	fclose(f);
	rename(tmpPath, BOOTLOG_PERSIST_PATH);
}

// Restore a log persisted by the previous launchd. Returns true if something
// was restored.
static bool restore_locked(void)
{
	struct stat st;
	if (stat(BOOTLOG_PERSIST_PATH, &st) != 0) return false;

	bool restored = false;
	time_t now = time(NULL);
	if (now - st.st_mtime >= 0 && now - st.st_mtime <= BOOTLOG_PERSIST_MAX_AGE) {
		FILE *f = fopen(BOOTLOG_PERSIST_PATH, "r");
		if (f) {
			char line[BOOTLOG_MAX_COLS + 8];
			while (fgets(line, sizeof(line), f)) {
				size_t len = strlen(line);
				while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
				if (len == 0) continue;
				char type = line[0];
				if (type == BOOTLOG_PERSIST_KMSG_ANCHOR) {
					// Continue the dmesg stream where the previous launchd left off
					strlcpy(g.kmsgLastLine, line + 1, sizeof(g.kmsgLastLine));
					continue;
				}
				if (type != BOOTLOG_ROW_TEXT && type != BOOTLOG_ROW_KERNEL && type != BOOTLOG_ROW_HEADER) continue;
				// Rows were already wrapped for this screen, push them as-is
				if (push_row(type, line + 1, len - 1)) {
					render_all();
				}
				restored = true;
			}
			fclose(f);
		}
	}
	unlink(BOOTLOG_PERSIST_PATH);
	return restored;
}

static void vprintf_locked(char type, bool timestamp, const char *fmt, va_list ap)
{
	char msg[BOOTLOG_MAX_COLS + 1];
	size_t prefixLen = 0;
	if (timestamp) {
		int r = snprintf(msg, sizeof(msg), "[%9.3f] ", uptime_seconds());
		if (r > 0) prefixLen = (size_t)r;
	}
	vsnprintf(msg + prefixLen, sizeof(msg) - prefixLen, fmt, ap);
	append_line_locked(type, msg);
}

static void printf_locked(char type, bool timestamp, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void printf_locked(char type, bool timestamp, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf_locked(type, timestamp, fmt, ap);
	va_end(ap);
}

// ---------------------------------------------------------------------------
// Kernel message buffer (dmesg)
// ---------------------------------------------------------------------------

static size_t read_kmsg(char *buf, size_t bufSize)
{
	// Preferred: proc_kmsgbuf, which is what dmesg(8) uses
	proc_kmsgbuf_t proc_kmsgbuf = resolve_proc_kmsgbuf();
	if (proc_kmsgbuf) {
		int r = proc_kmsgbuf(buf, (uint32_t)(bufSize - 1));
		if (r > 0) {
			size_t n = (size_t)r;
			if (n > bufSize - 1) n = bufSize - 1;
			buf[n] = '\0';
			return n;
		}
	}

	// Fallback: kern.msgbuf sysctl
	size_t len = bufSize - 1;
	if (sysctlbyname("kern.msgbuf", buf, &len, NULL, 0) == 0 && len > 0) {
		buf[len] = '\0';
		return len;
	}

	buf[0] = '\0';
	return 0;
}

// Prints kernel messages that appeared since the last poll. On the first poll
// only the last BOOTLOG_KMSG_INITIAL_LINES lines are shown.
static void poll_kmsg_locked(bool force)
{
	uint64_t now = monotonic_ns();
	if (!force && (now - g.kmsgLastPoll) < BOOTLOG_KMSG_POLL_INTERVAL_NS) return;
	g.kmsgLastPoll = now;

	if (!g.kmsgBuf) {
		g.kmsgBuf = malloc(BOOTLOG_KMSG_BUFSIZE);
		if (!g.kmsgBuf) return;
	}

	char *buf = g.kmsgBuf;
	size_t n = read_kmsg(buf, BOOTLOG_KMSG_BUFSIZE);
	if (n == 0) return;

	// The buffer may contain embedded NULs, treat them as line breaks
	for (size_t i = 0; i < n; i++) {
		if (buf[i] == '\0') buf[i] = '\n';
	}

	char *start = NULL;
	if (g.kmsgLastLine[0]) {
		// Find the last occurrence of the last line we printed, everything
		// after it is new
		char *found = NULL;
		char *p = buf;
		size_t lastLen = strlen(g.kmsgLastLine);
		while ((p = strstr(p, g.kmsgLastLine)) != NULL) {
			found = p;
			p += 1;
		}
		if (found) {
			char *nl = strchr(found + lastLen, '\n');
			start = nl ? nl + 1 : buf + n;
		}
	}

	if (!start) {
		// First poll (or the buffer wrapped so far that we lost our anchor):
		// show the tail only
		int lines = 0;
		start = buf + n;
		while (start > buf) {
			if (*(start - 1) == '\n') {
				if (++lines > BOOTLOG_KMSG_INITIAL_LINES) break;
			}
			start--;
		}
	}

	// Print line by line
	char *p = start;
	while (p < buf + n) {
		char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);

		// Strip trailing whitespace
		while (len > 0 && isspace((unsigned char)p[len - 1])) len--;

		if (len > 0) {
			char saved = p[len];
			p[len] = '\0';
			append_line_locked(BOOTLOG_ROW_KERNEL, p);
			strlcpy(g.kmsgLastLine, p, sizeof(g.kmsgLastLine));
			p[len] = saved;
		}

		if (!nl) break;
		p = nl + 1;
	}
}

// ---------------------------------------------------------------------------
// Framebuffer setup
// ---------------------------------------------------------------------------

static int setup_display_locked(void)
{
	g.ctx = drawctx_init();
	if (!g.ctx) return -1;

	g.fbWidth = (int)g.ctx->size.width;
	g.fbHeight = (int)g.ctx->size.height;
	g.fbStride = g.ctx->bytesPerRow / 4;
	g.shadowSize = (size_t)g.fbHeight * (size_t)g.ctx->bytesPerRow;
	if (g.fbWidth <= 0 || g.fbHeight <= 0 || g.fbStride < g.fbWidth || g.shadowSize == 0) {
		drawctx_free(g.ctx);
		g.ctx = NULL;
		return -1;
	}

	g.shadow = malloc(g.shadowSize);
	if (!g.shadow) {
		drawctx_free(g.ctx);
		g.ctx = NULL;
		return -1;
	}
	memset(g.shadow, 0, g.shadowSize);

	g.rotation = (int)get_main_screen_rotation();
	if (g.rotation == 90 || g.rotation == 270) {
		g.logicalWidth = g.fbHeight;
		g.logicalHeight = g.fbWidth;
	}
	else {
		g.logicalWidth = g.fbWidth;
		g.logicalHeight = g.fbHeight;
	}

	// Pick a font scale that gives roughly 60-100 columns
	// (750px -> 1x, 1170px -> 2x, 2048px -> 3x)
	g.scale = (g.logicalWidth + 320) / 640;
	if (g.scale < 1) g.scale = 1;
	g.cellWidth = BOOTLOG_FONT_WIDTH * g.scale;
	g.cellHeight = BOOTLOG_FONT_HEIGHT * g.scale;

	// Keep a little distance from the edges (and the notch / dynamic island)
	int topInset = g.logicalHeight / 16;
	int sideInset = g.cellWidth;
	int bottomInset = g.cellHeight;

	g.cols = (g.logicalWidth - 2 * sideInset) / g.cellWidth;
	g.rows = (g.logicalHeight - topInset - bottomInset) / g.cellHeight;
	if (g.cols > BOOTLOG_MAX_COLS) g.cols = BOOTLOG_MAX_COLS;
	if (g.cols < 8 || g.rows < 2) {
		free(g.shadow);
		g.shadow = NULL;
		drawctx_free(g.ctx);
		g.ctx = NULL;
		return -1;
	}
	g.originX = (g.logicalWidth - g.cols * g.cellWidth) / 2;
	g.originY = topInset;

	g.ring = calloc((size_t)g.rows, sizeof(struct bootlog_row));
	if (!g.ring) {
		free(g.shadow);
		g.shadow = NULL;
		drawctx_free(g.ctx);
		g.ctx = NULL;
		return -1;
	}
	g.ringHead = 0;
	g.ringCount = 0;
	return 0;
}

static void teardown_locked(void)
{
	if (g.ctx) {
		struct drawctx *ctx = g.ctx;
		exec_with_asl_disabled(^{
			drawctx_free(ctx);
		});
		g.ctx = NULL;
	}
	if (g.shadow) {
		free(g.shadow);
		g.shadow = NULL;
	}
	if (g.ring) {
		free(g.ring);
		g.ring = NULL;
	}
	if (g.kmsgBuf) {
		free(g.kmsgBuf);
		g.kmsgBuf = NULL;
	}
	g.kmsgLastLine[0] = '\0';
	g.kmsgLastPoll = 0;
	g.active = false;
	g.persist = false;
}

static void print_header_locked(void)
{
	char version[512];
	char bootargs[512];
	char machine[64];
	char model[64];

	// The very first thing the real verbose boot prints is the kernel banner
	if (sysctl_string("kern.version", version, sizeof(version)) == 0) {
		printf_locked(BOOTLOG_ROW_HEADER, false, "%s", version);
	}
	if (sysctl_string("kern.bootargs", bootargs, sizeof(bootargs)) == 0) {
		printf_locked(BOOTLOG_ROW_HEADER, false, "boot-args: %s", bootargs[0] ? bootargs : "(none)");
	}
	sysctl_string("hw.machine", machine, sizeof(machine));
	sysctl_string("hw.model", model, sizeof(model));
	printf_locked(BOOTLOG_ROW_HEADER, false, "hardware: %s (%s)", machine[0] ? machine : "?", model[0] ? model : "?");
	const char *rootPath = jbinfo(rootPath);
	printf_locked(BOOTLOG_ROW_HEADER, false, "Dopamine: launchdhook loaded, jbroot=%s", rootPath ? rootPath : "?");
	printf_locked(BOOTLOG_ROW_HEADER, false, "%s", "");

	// Followed by whatever the kernel logged so far
	poll_kmsg_locked(true);
	printf_locked(BOOTLOG_ROW_TEXT, false, "%s", "");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int bootlog_start(bool beforeUserspaceReboot)
{
	__block int result = 0;
	pthread_mutex_lock(&gLock);
	if (g.active) {
		pthread_mutex_unlock(&gLock);
		return 0;
	}

	exec_with_asl_disabled(^{
		result = setup_display_locked();
	});
	if (result != 0) {
		pthread_mutex_unlock(&gLock);
		return -1;
	}

	g.active = true;
	g.persist = beforeUserspaceReboot;

	bool restored = false;
	if (!beforeUserspaceReboot) {
		restored = restore_locked();
	}
	else {
		// A stale file from a previous attempt must not be picked up later
		unlink(BOOTLOG_PERSIST_PATH);
	}

	if (!restored) {
		print_header_locked();
	}
	if (beforeUserspaceReboot) {
		printf_locked(BOOTLOG_ROW_TEXT, true, "launchd[1]: userspace reboot requested (kern.willuserspacereboot)");
		printf_locked(BOOTLOG_ROW_TEXT, true, "launchd[1]: tearing down userspace...");
	}
	else {
		printf_locked(BOOTLOG_ROW_TEXT, true, "launchd[1]: userspace reboot: launchd re-executed (pid %d)", getpid());
	}

	flush();
	persist_locked();
	pthread_mutex_unlock(&gLock);
	return 0;
}

bool bootlog_is_active(void)
{
	pthread_mutex_lock(&gLock);
	bool active = g.active;
	pthread_mutex_unlock(&gLock);
	return active;
}

void bootlog_printf(const char *fmt, ...)
{
	pthread_mutex_lock(&gLock);
	if (g.active) {
		va_list ap;
		va_start(ap, fmt);
		vprintf_locked(BOOTLOG_ROW_TEXT, true, fmt, ap);
		va_end(ap);
		flush();
		persist_locked();
	}
	pthread_mutex_unlock(&gLock);
}

void bootlog_spawn_event(const char *path, char *const argv[])
{
	if (!path) return;

	pthread_mutex_lock(&gLock);
	if (g.active) {
		// Kernel messages first, they happened before this spawn
		poll_kmsg_locked(false);

		if (!strcmp(path, "/usr/libexec/xpcproxy") && argv && argv[0] && argv[1]) {
			// launchd passes the label as argv[1], with a trailing newline
			char label[256];
			strlcpy(label, argv[1], sizeof(label));
			size_t len = strlen(label);
			while (len > 0 && (label[len - 1] == '\n' || label[len - 1] == '\r')) label[--len] = '\0';
			printf_locked(BOOTLOG_ROW_TEXT, true, "launchd[1]: spawn %s", label);
		}
		else {
			printf_locked(BOOTLOG_ROW_TEXT, true, "launchd[1]: exec %s", path);
		}

		flush();
		persist_locked();
	}
	pthread_mutex_unlock(&gLock);
}

void bootlog_stop(void)
{
	pthread_mutex_lock(&gLock);
	if (g.active) {
		teardown_locked();
	}
	pthread_mutex_unlock(&gLock);
}
