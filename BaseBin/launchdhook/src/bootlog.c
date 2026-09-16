#include "bootlog.h"
#include "bootlog_font.h"

#include <libjailbreak/display.h>
#include <libjailbreak/info.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <ctype.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <dispatch/dispatch.h>

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

// Longest line we keep (longer input is cut)
#define BOOTLOG_MAX_LINE_LEN 1024
// Lines kept for the log file (oldest are dropped beyond this)
#define BOOTLOG_MAX_HISTORY 4096

// Where the log is stashed across the launchd re-exec
#define BOOTLOG_PERSIST_PATH "/private/var/tmp/.dopamine_bootlog"
// Ignore a stashed log older than this (seconds), it is from another boot
#define BOOTLOG_PERSIST_MAX_AGE 180

// Where the complete log of the last userspace reboot ends up
#define BOOTLOG_LOGFILE_DIR "/var/mobile/Library/Logs/Dopamine"
#define BOOTLOG_LOGFILE_PATH BOOTLOG_LOGFILE_DIR "/bootlog.txt"

// Size of the buffer used to read the kernel message buffer
#define BOOTLOG_KMSG_BUFSIZE (256 * 1024)
// How many dmesg lines to show initially
#define BOOTLOG_KMSG_INITIAL_LINES 24
// Minimum interval between two dmesg polls
#define BOOTLOG_KMSG_POLL_INTERVAL_NS (150ull * NSEC_PER_MSEC)

// Framebuffer swaps are coalesced to at most one per this interval
#define BOOTLOG_FLUSH_INTERVAL_NS (40ull * NSEC_PER_MSEC)

// Safety net: the log stops itself after this many seconds no matter what,
// so it can never keep painting over SpringBoard
#define BOOTLOG_WATCHDOG_SECONDS 120

// When to stop drawing
//
// backboardd owns the display once it is up, but it does not put anything on
// screen until SpringBoard has rendered its first frame (seconds later). Until
// then our swaps are harmless, so the log keeps scrolling through the whole
// SpringBoard launch. The moment SpringBoard is done launching has to be
// observed from a normal process (launchd can't use libnotify), which is what
// the com.opa334.Dopamine.bootlog launch daemon (`jbctl internal bootlog_watch`)
// does: it waits for SpringBoard's "finished launching" notification and then
// drops BOOTLOG_STOP_MARKER_PATH, which launchd polls for.
//
// If that daemon is not installed, the log falls back to stopping as soon as
// backboardd is spawned. If it is installed but never reports back, the hard
// cap below stops the log some seconds after backboardd was spawned.
#define BOOTLOG_WATCHER_PLIST_RELPATH "/basebin/LaunchDaemons/com.opa334.Dopamine.bootlog.plist"
#define BOOTLOG_ACTIVE_MARKER_PATH "/private/var/tmp/.dopamine_bootlog_active"
#define BOOTLOG_STOP_MARKER_PATH "/private/var/tmp/.dopamine_bootlog_stop"
#define BOOTLOG_STOP_MARKER_POLL_MS 25
#define BOOTLOG_BACKBOARDD_HARD_CAP_MS 20000

// Second line of defense, independent of any daemon: the swap ids that
// IOMobileFramebufferSwapBegin hands out are a per-display counter. As long as
// only we swap, every swap of ours gets the previous id + 1. The moment
// backboardd presents SpringBoard's first frame, our next swap sees a gap, and
// we stop on the spot (that one frame of ours is the only thing that can ever
// end up over SpringBoard). Whether the ids really are per-display is verified
// at startup by looking at the ids of our own two surfaces.

// ---------------------------------------------------------------------------
// Colors
// ---------------------------------------------------------------------------

// Framebuffer pixel format is 32 bit BGRA little endian -> 0xAARRGGBB as uint32
enum bootlog_color {
	COL_BG = 0,        // black
	COL_DIM,           // timestamps
	COL_TEXT,          // plain launchd text
	COL_KERNEL,        // kernel messages
	COL_KERNEL_TAG,    // "Subsystem:" prefix of kernel messages
	COL_HEADER,        // kernel banner / hardware info
	COL_TAG_LAUNCHD,   // "launchd[1]:"
	COL_TAG_DOPAMINE,  // "Dopamine:"
	COL_LABEL,         // launchd labels and executable paths
	COL_MILESTONE,     // reboot / re-exec / handover lines
	COL_WARN,
	COL_ERROR,
	COL_COUNT
};

static const uint32_t gPalette[COL_COUNT] = {
	[COL_BG]           = 0xFF000000u,
	[COL_DIM]          = 0xFF7A7A7Au,
	[COL_TEXT]         = 0xFFE8E8E8u,
	[COL_KERNEL]       = 0xFFB4B4B4u,
	[COL_KERNEL_TAG]   = 0xFF9FD3E6u,
	[COL_HEADER]       = 0xFF8CB8FFu,
	[COL_TAG_LAUNCHD]  = 0xFF5FD7FFu,
	[COL_TAG_DOPAMINE] = 0xFFD787FFu,
	[COL_LABEL]        = 0xFF8CE68Cu,
	[COL_MILESTONE]    = 0xFFFFD75Fu,
	[COL_WARN]         = 0xFFFFD75Fu,
	[COL_ERROR]        = 0xFFFF6B6Bu,
};

// Line categories (also the first byte of each persisted line)
#define CAT_HEADER   'H'
#define CAT_KERNEL   'K'
#define CAT_LAUNCHD  'L'
#define CAT_DOPAMINE 'D'
// Persisted file only: the last kernel message that was already shown
#define PERSIST_KMSG_ANCHOR 'A'

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct bootlog_line {
	char category;
	char *text;
};

static struct {
	bool active;
	bool persist;
	uint32_t generation;  // bumped on every stop so stale dispatch blocks bail out

	struct drawctx *ctx[2]; // two surfaces on the display, we alternate between them (double buffering)
	int ctxCount;
	int nextCtx;
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

	struct bootlog_line *lines;  // everything logged since the start (tail is on screen)
	int lineCount;
	int lineCap;

	bool dirty;           // shadow buffer needs re-rendering
	bool flushPending;    // a deferred flush is scheduled
	bool watcherAvailable; // the bootlog launch daemon is installed and will tell us when SpringBoard is up
	bool hardCapArmed;    // backboardd was spawned, the hard cap timer is running
	uint64_t lastFlush;

	bool swapIdsGlobal;   // swap ids are a per-display counter, foreign swaps can be detected
	int firstSwapToken;
	int lastSwapToken;
	int foreignSwaps;     // gaps seen in the swap id sequence

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
	return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

// Same clock the kernel stamps its message buffer with (includes time spent
// asleep), so our lines line up with the dmesg lines around them
static uint64_t kernel_clock_ns(void)
{
	static mach_timebase_info_data_t timebase;
	if (timebase.denom == 0) {
		mach_timebase_info(&timebase);
		if (timebase.denom == 0) {
			timebase.numer = 1;
			timebase.denom = 1;
		}
	}
	uint64_t t = mach_continuous_time();
	return t * timebase.numer / timebase.denom;
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

static bool is_word_char(char c)
{
	return isalnum((unsigned char)c) != 0;
}

// Case insensitive search for `word` in `text` where the match must not be
// glued to other letters/digits ("failed" matches "Failed updating", but not
// "_failedCommand:0"). Returns the offset or -1.
static int find_word(const char *text, const char *word)
{
	size_t wordLen = strlen(word);
	size_t textLen = strlen(text);
	if (wordLen == 0 || textLen < wordLen) return -1;
	for (size_t i = 0; i + wordLen <= textLen; i++) {
		if (strncasecmp(text + i, word, wordLen) != 0) continue;
		if (i > 0 && is_word_char(text[i - 1])) continue;
		if (i + wordLen < textLen && is_word_char(text[i + wordLen])) continue;
		return (int)i;
	}
	return -1;
}

static bool line_looks_like_error(const char *text)
{
	static const char *words[] = { "panic", "error", "failed", "failure", "fault", "denied", "abort", "assert", "cannot" };
	for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		if (find_word(text, words[i]) >= 0) return true;
	}
	return false;
}

static bool line_looks_like_warning(const char *text)
{
	static const char *words[] = { "warn", "warning", "timeout", "timed out", "retry", "deprecated" };
	for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		if (find_word(text, words[i]) >= 0) return true;
	}
	return false;
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
	g.shadow[(size_t)py * (size_t)g.fbStride + (size_t)px] = color;
}

static void draw_cell(int col, int row, char ch, uint32_t fg)
{
	int glyphIndex = 0; // space
	if (ch >= BOOTLOG_FONT_FIRST_CHAR && ch <= BOOTLOG_FONT_LAST_CHAR) {
		glyphIndex = ch - BOOTLOG_FONT_FIRST_CHAR;
	}
	const uint8_t *glyph = gBootlogFont[glyphIndex];
	const uint32_t bg = gPalette[COL_BG];

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
				uint32_t *line = &g.shadow[(size_t)py * (size_t)g.fbStride];
				for (int b = 0; b < BOOTLOG_FONT_WIDTH; b++) {
					uint32_t color = (bits & (0x80 >> b)) ? fg : bg;
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
			uint32_t color = (bits & (0x80 >> b)) ? fg : bg;
			for (int sy = 0; sy < scale; sy++) {
				for (int sx = 0; sx < scale; sx++) {
					put_pixel(x0 + b * scale + sx, y0 + r * scale + sy, color);
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Colorizer: assigns a palette index to every character of a line
// ---------------------------------------------------------------------------

static void fill_colors(uint8_t *colors, size_t from, size_t to, uint8_t color)
{
	for (size_t i = from; i < to; i++) colors[i] = color;
}

static void colorize_line(const struct bootlog_line *line, uint8_t *colors, size_t len)
{
	const char *text = line->text;
	size_t pos = 0;

	uint8_t base = COL_TEXT;
	switch (line->category) {
		case CAT_HEADER: base = COL_HEADER; break;
		case CAT_KERNEL: base = COL_KERNEL; break;
		default: base = COL_TEXT; break;
	}
	fill_colors(colors, 0, len, base);
	if (len == 0) return;

	// "[ 1234.567890]" / "[ 1234.567890]: " timestamp prefix -> dim
	if (text[0] == '[') {
		const char *close = strchr(text, ']');
		if (close && (size_t)(close - text) <= 20) {
			size_t end = (size_t)(close - text) + 1;
			if (end < len && text[end] == ':') end++;
			while (end < len && text[end] == ' ') end++;
			fill_colors(colors, 0, end, COL_DIM);
			pos = end;
		}
	}

	const char *msg = text + pos;
	size_t msgLen = len - pos;

	if (line->category == CAT_HEADER) {
		return;
	}

	if (line->category == CAT_KERNEL) {
		if (line_looks_like_error(msg)) {
			fill_colors(colors, pos, len, COL_ERROR);
			return;
		}
		if (line_looks_like_warning(msg)) {
			fill_colors(colors, pos, len, COL_WARN);
			return;
		}
		// "Subsystem: message" -> highlight the subsystem
		const char *colon = strstr(msg, ": ");
		if (colon && (size_t)(colon - msg) > 0 && (size_t)(colon - msg) <= 48) {
			fill_colors(colors, pos, pos + (size_t)(colon - msg) + 1, COL_KERNEL_TAG);
		}
		return;
	}

	// launchd / Dopamine lines: "tag: message"
	uint8_t tagColor = (line->category == CAT_DOPAMINE) ? COL_TAG_DOPAMINE : COL_TAG_LAUNCHD;
	const char *colon = strstr(msg, ": ");
	size_t bodyStart = pos;
	if (colon && (size_t)(colon - msg) <= 24) {
		size_t tagEnd = pos + (size_t)(colon - msg) + 1;
		fill_colors(colors, pos, tagEnd, tagColor);
		bodyStart = tagEnd;
		while (bodyStart < len && text[bodyStart] == ' ') bodyStart++;
	}
	const char *body = text + bodyStart;
	size_t bodyLen = len - bodyStart;

	if (line_looks_like_error(body)) {
		fill_colors(colors, bodyStart, len, COL_ERROR);
		return;
	}
	if (line->category == CAT_LAUNCHD) {
		// "spawn <label>" / "exec <path>" -> highlight what got launched
		if (bodyLen > 6 && !strncmp(body, "spawn ", 6)) {
			fill_colors(colors, bodyStart + 6, len, COL_LABEL);
			return;
		}
		if (bodyLen > 5 && !strncmp(body, "exec ", 5)) {
			fill_colors(colors, bodyStart + 5, len, COL_LABEL);
			return;
		}
		// Everything else launchd says during a userspace reboot is a milestone
		fill_colors(colors, bodyStart, len, COL_MILESTONE);
		return;
	}
	if (line_looks_like_warning(body)) {
		fill_colors(colors, bodyStart, len, COL_WARN);
	}
	(void)msgLen;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static int rows_for_line(const struct bootlog_line *line)
{
	size_t len = strlen(line->text);
	if (len == 0) return 1;
	return (int)((len + (size_t)g.cols - 1) / (size_t)g.cols);
}

static void render_blank_row(int row)
{
	for (int col = 0; col < g.cols; col++) {
		draw_cell(col, row, ' ', gPalette[COL_TEXT]);
	}
}

// Renders one logical line starting at visual row `row`, returns the number of
// rows used. Rows beyond the screen are skipped.
static int render_line(const struct bootlog_line *line, int row)
{
	size_t len = strlen(line->text);
	uint8_t colorsStack[BOOTLOG_MAX_LINE_LEN + 1];
	uint8_t *colors = colorsStack;
	if (len > BOOTLOG_MAX_LINE_LEN) len = BOOTLOG_MAX_LINE_LEN;
	colorize_line(line, colors, len);

	int used = 0;
	size_t offset = 0;
	do {
		if (row + used >= g.rows) break;
		for (int col = 0; col < g.cols; col++) {
			size_t idx = offset + (size_t)col;
			char ch = (idx < len) ? line->text[idx] : ' ';
			uint32_t fg = (idx < len) ? gPalette[colors[idx]] : gPalette[COL_TEXT];
			draw_cell(col, row + used, ch, fg);
		}
		used++;
		offset += (size_t)g.cols;
	} while (offset < len);
	return used;
}

// Re-render the whole screen from the tail of the line history
static void render_all(void)
{
	// Walk backwards from the newest line and take as many as fit
	int first = g.lineCount;
	int total = 0;
	for (int i = g.lineCount - 1; i >= 0; i--) {
		int r = rows_for_line(&g.lines[i]);
		if (total + r > g.rows) break;
		total += r;
		first = i;
	}
	if (first == g.lineCount && g.lineCount > 0) {
		// The newest line alone is taller than the screen, show what fits
		first = g.lineCount - 1;
	}

	int row = 0;
	for (int i = first; i < g.lineCount && row < g.rows; i++) {
		row += render_line(&g.lines[i], row);
	}
	for (; row < g.rows; row++) {
		render_blank_row(row);
	}
	g.dirty = false;
}

static void stop_locked(const char *reason, bool finalFlush);

static void flush_now_locked(void)
{
	if (g.ctxCount == 0 || !g.shadow) return;
	if (g.dirty) render_all();
	// Always draw into the surface that is *not* currently on screen, so the
	// display never scans out a half copied frame
	struct drawctx *target = g.ctx[g.nextCtx];
	g.nextCtx = (g.nextCtx + 1) % g.ctxCount;
	exec_with_asl_disabled(^{
		drawctx_draw_raw(target, g.shadow, g.shadowSize);
	});
	g.lastFlush = monotonic_ns();

	// Did somebody else swap since our previous swap? Then the display is no
	// longer ours (only checked after the re-exec, the old launchd's display is
	// being torn down around us and that is expected to be noisy).
	int token = target->lastSwapToken;
	int delta = token - g.lastSwapToken;
	bool tokensValid = (token > 0 && g.lastSwapToken > 0);
	g.lastSwapToken = token;
	if (g.swapIdsGlobal && !g.persist && tokensValid && delta >= 2 && delta <= 10000) {
		g.foreignSwaps++;
		char reason[160];
		snprintf(reason, sizeof(reason), "display taken over by someone else (swap id jumped %d -> %d)", token - delta, token);
		stop_locked(reason, false);
	}
}

// Coalesces framebuffer swaps: flush right away if the last one is old
// enough, otherwise schedule one so the latest lines still show up shortly.
static void request_flush_locked(void)
{
	uint64_t now = monotonic_ns();
	uint64_t elapsed = now - g.lastFlush;
	if (g.lastFlush == 0 || elapsed >= BOOTLOG_FLUSH_INTERVAL_NS) {
		flush_now_locked();
		return;
	}
	if (g.flushPending) return;
	g.flushPending = true;

	uint32_t generation = g.generation;
	uint64_t delay = BOOTLOG_FLUSH_INTERVAL_NS - elapsed;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)delay), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		pthread_mutex_lock(&gLock);
		if (g.active && g.generation == generation) {
			g.flushPending = false;
			flush_now_locked();
		}
		pthread_mutex_unlock(&gLock);
	});
}

// ---------------------------------------------------------------------------
// Line history
// ---------------------------------------------------------------------------

static void history_push(char category, const char *text)
{
	if (g.lineCount >= BOOTLOG_MAX_HISTORY) {
		// Drop the oldest quarter
		int drop = BOOTLOG_MAX_HISTORY / 4;
		for (int i = 0; i < drop; i++) free(g.lines[i].text);
		memmove(g.lines, g.lines + drop, (size_t)(g.lineCount - drop) * sizeof(struct bootlog_line));
		g.lineCount -= drop;
	}
	if (g.lineCount >= g.lineCap) {
		int newCap = g.lineCap ? g.lineCap * 2 : 256;
		struct bootlog_line *newLines = realloc(g.lines, (size_t)newCap * sizeof(struct bootlog_line));
		if (!newLines) return;
		g.lines = newLines;
		g.lineCap = newCap;
	}
	char *copy = strdup(text);
	if (!copy) return;
	g.lines[g.lineCount].category = category;
	g.lines[g.lineCount].text = copy;
	g.lineCount++;
	g.dirty = true;
}

static void history_free(void)
{
	for (int i = 0; i < g.lineCount; i++) free(g.lines[i].text);
	free(g.lines);
	g.lines = NULL;
	g.lineCount = 0;
	g.lineCap = 0;
}

// Sanitizes a line (printable ASCII only, tabs expanded, trailing whitespace
// removed) and appends it. Does not flush.
static void append_line_locked(char category, const char *line)
{
	char clean[BOOTLOG_MAX_LINE_LEN + 1];
	size_t n = 0;
	for (const char *p = line; *p && n < BOOTLOG_MAX_LINE_LEN; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == '\t') {
			for (int i = 0; i < 4 && n < BOOTLOG_MAX_LINE_LEN; i++) clean[n++] = ' ';
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
	while (n > 0 && clean[n - 1] == ' ') n--;
	clean[n] = '\0';
	history_push(category, clean);
}

static void persist_locked(void)
{
	if (!g.persist) return;

	char tmpPath[] = BOOTLOG_PERSIST_PATH ".tmp";
	FILE *f = fopen(tmpPath, "w");
	if (!f) return;
	if (g.kmsgLastLine[0]) {
		fputc(PERSIST_KMSG_ANCHOR, f);
		fputs(g.kmsgLastLine, f);
		fputc('\n', f);
	}
	for (int i = 0; i < g.lineCount; i++) {
		fputc(g.lines[i].category, f);
		fputs(g.lines[i].text, f);
		fputc('\n', f);
	}
	fclose(f);
	rename(tmpPath, BOOTLOG_PERSIST_PATH);
}

// Restore the log persisted by the previous launchd. Returns true if something
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
			char line[BOOTLOG_MAX_LINE_LEN + 8];
			while (fgets(line, sizeof(line), f)) {
				size_t len = strlen(line);
				while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
				if (len == 0) continue;
				char type = line[0];
				if (type == PERSIST_KMSG_ANCHOR) {
					// Continue the dmesg stream where the previous launchd left off
					strlcpy(g.kmsgLastLine, line + 1, sizeof(g.kmsgLastLine));
					continue;
				}
				if (type != CAT_HEADER && type != CAT_KERNEL && type != CAT_LAUNCHD && type != CAT_DOPAMINE) continue;
				history_push(type, line + 1);
				restored = true;
			}
			fclose(f);
		}
	}
	unlink(BOOTLOG_PERSIST_PATH);
	return restored;
}

// Writes the complete log to the log file (best effort)
static void write_logfile_locked(const char *reason)
{
	mkdir(BOOTLOG_LOGFILE_DIR, 0755);
	FILE *f = fopen(BOOTLOG_LOGFILE_PATH, "w");
	if (!f) return;
	for (int i = 0; i < g.lineCount; i++) {
		fputs(g.lines[i].text, f);
		fputc('\n', f);
	}
	if (reason) {
		fprintf(f, "\n-- bootlog stopped: %s --\n", reason);
	}
	fprintf(f, "-- swap ids: %s (first %d, last %d, gaps seen %d); bootlog daemon %s --\n",
		g.swapIdsGlobal ? "per-display counter" : "not usable for takeover detection",
		g.firstSwapToken, g.lastSwapToken, g.foreignSwaps,
		g.watcherAvailable ? "installed" : "not installed");
	fclose(f);
	chmod(BOOTLOG_LOGFILE_PATH, 0644);
}

static void vprintf_locked(char category, bool timestamp, const char *fmt, va_list ap)
{
	char msg[BOOTLOG_MAX_LINE_LEN + 1];
	size_t prefixLen = 0;
	if (timestamp) {
		uint64_t ns = kernel_clock_ns();
		int r = snprintf(msg, sizeof(msg), "[%5llu.%06llu] ", (unsigned long long)(ns / NSEC_PER_SEC), (unsigned long long)((ns % NSEC_PER_SEC) / 1000));
		if (r > 0) prefixLen = (size_t)r;
	}
	vsnprintf(msg + prefixLen, sizeof(msg) - prefixLen, fmt, ap);
	append_line_locked(category, msg);
}

static void printf_locked(char category, bool timestamp, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void printf_locked(char category, bool timestamp, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf_locked(category, timestamp, fmt, ap);
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

// Appends kernel messages that appeared since the last poll. On the first poll
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

	char *p = start;
	while (p < buf + n) {
		char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);

		while (len > 0 && isspace((unsigned char)p[len - 1])) len--;

		if (len > 0) {
			char saved = p[len];
			p[len] = '\0';
			append_line_locked(CAT_KERNEL, p);
			strlcpy(g.kmsgLastLine, p, sizeof(g.kmsgLastLine));
			p[len] = saved;
		}

		if (!nl) break;
		p = nl + 1;
	}
}

// ---------------------------------------------------------------------------
// Framebuffer setup / teardown
// ---------------------------------------------------------------------------

static void release_display_locked(void)
{
	for (int i = 0; i < g.ctxCount; i++) {
		drawctx_free(g.ctx[i]);
		g.ctx[i] = NULL;
	}
	g.ctxCount = 0;
	g.nextCtx = 0;
	if (g.shadow) {
		free(g.shadow);
		g.shadow = NULL;
	}
}

static int setup_display_locked(void)
{
	struct drawctx *front = drawctx_init();
	if (!front) return -1;
	g.ctx[0] = front;
	g.ctxCount = 1;
	g.nextCtx = 0;

	g.fbWidth = (int)front->size.width;
	g.fbHeight = (int)front->size.height;
	g.fbStride = front->bytesPerRow / 4;
	g.shadowSize = (size_t)g.fbHeight * (size_t)front->bytesPerRow;
	if (g.fbWidth <= 0 || g.fbHeight <= 0 || g.fbStride < g.fbWidth || g.shadowSize == 0) {
		release_display_locked();
		return -1;
	}

	// Second surface for double buffering (optional, we cope without it)
	struct drawctx *back = drawctx_init();
	if (back) {
		if ((int)back->size.width == g.fbWidth && (int)back->size.height == g.fbHeight && back->bytesPerRow == front->bytesPerRow) {
			g.ctx[1] = back;
			g.ctxCount = 2;
		}
		else {
			drawctx_free(back);
		}
	}

	// Both inits swapped once. If the ids are a per-display counter, the second
	// one is right after the first (allow a little slack for a stray swap of a
	// dying process in between). Independent per-client counters would hand
	// out the same id twice.
	g.firstSwapToken = front->lastSwapToken;
	g.lastSwapToken = front->lastSwapToken;
	g.swapIdsGlobal = false;
	g.foreignSwaps = 0;
	if (g.ctxCount == 2 && front->lastSwapToken > 0 && g.ctx[1]->lastSwapToken > 0) {
		int gap = g.ctx[1]->lastSwapToken - front->lastSwapToken;
		g.swapIdsGlobal = (gap >= 1 && gap <= 3);
		g.lastSwapToken = g.ctx[1]->lastSwapToken;
	}

	g.shadow = malloc(g.shadowSize);
	if (!g.shadow) {
		release_display_locked();
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
	if (g.cols > BOOTLOG_MAX_LINE_LEN) g.cols = BOOTLOG_MAX_LINE_LEN;
	if (g.cols < 8 || g.rows < 2) {
		release_display_locked();
		return -1;
	}
	g.originX = (g.logicalWidth - g.cols * g.cellWidth) / 2;
	g.originY = topInset;
	return 0;
}

static void teardown_locked(void)
{
	g.generation++;
	exec_with_asl_disabled(^{
		release_display_locked();
	});
	history_free();
	if (g.kmsgBuf) {
		free(g.kmsgBuf);
		g.kmsgBuf = NULL;
	}
	g.kmsgLastLine[0] = '\0';
	g.kmsgLastPoll = 0;
	g.dirty = false;
	g.flushPending = false;
	g.watcherAvailable = false;
	g.hardCapArmed = false;
	g.lastFlush = 0;
	g.active = false;
	g.persist = false;
	unlink(BOOTLOG_ACTIVE_MARKER_PATH);
	unlink(BOOTLOG_STOP_MARKER_PATH);
}

// finalFlush: whether to put the "stopped" line on screen. Must be false when
// SpringBoard may already be visible, one more swap would flash over it.
static void stop_locked(const char *reason, bool finalFlush)
{
	if (!g.active) return;
	printf_locked(CAT_LAUNCHD, true, "launchd[1]: boot log stopped (%s)", reason ? reason : "no reason");
	if (finalFlush) flush_now_locked();
	write_logfile_locked(reason);
	teardown_locked();
}

static void arm_backboardd_hard_cap_locked(void)
{
	if (g.hardCapArmed) return;
	g.hardCapArmed = true;
	uint32_t generation = g.generation;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)BOOTLOG_BACKBOARDD_HARD_CAP_MS * (int64_t)NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		pthread_mutex_lock(&gLock);
		if (g.active && g.generation == generation) {
			stop_locked("hard cap after backboardd spawn, the bootlog daemon never reported SpringBoard", false);
		}
		pthread_mutex_unlock(&gLock);
	});
}

// Polls for the marker the bootlog daemon drops once SpringBoard finished launching
static void poll_stop_marker(uint32_t generation)
{
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)BOOTLOG_STOP_MARKER_POLL_MS * (int64_t)NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		pthread_mutex_lock(&gLock);
		if (!g.active || g.generation != generation) {
			pthread_mutex_unlock(&gLock);
			return;
		}
		if (access(BOOTLOG_STOP_MARKER_PATH, F_OK) == 0) {
			char reason[256] = "bootlog daemon asked us to stop";
			FILE *f = fopen(BOOTLOG_STOP_MARKER_PATH, "r");
			if (f) {
				char line[256];
				if (fgets(line, sizeof(line), f)) {
					size_t len = strlen(line);
					while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
					if (len > 0) strlcpy(reason, line, sizeof(reason));
				}
				fclose(f);
			}
			stop_locked(reason, false);
			pthread_mutex_unlock(&gLock);
			return;
		}
		pthread_mutex_unlock(&gLock);
		poll_stop_marker(generation);
	});
}

// Called once in the re-executed launchd: figures out whether the bootlog
// launch daemon is there and, if so, starts waiting for its signal
static void setup_springboard_watch_locked(void)
{
	unlink(BOOTLOG_STOP_MARKER_PATH);
	g.watcherAvailable = false;

	const char *rootPath = jbinfo(rootPath);
	if (!rootPath) return;
	char plistPath[PATH_MAX];
	snprintf(plistPath, sizeof(plistPath), "%s%s", rootPath, BOOTLOG_WATCHER_PLIST_RELPATH);
	if (access(plistPath, F_OK) != 0) {
		printf_locked(CAT_DOPAMINE, true, "Dopamine: bootlog daemon not installed, log will stop when backboardd starts");
		return;
	}

	// Tell the daemon that there is a log to stop this boot
	int fd = open(BOOTLOG_ACTIVE_MARKER_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return;
	close(fd);

	g.watcherAvailable = true;
	poll_stop_marker(g.generation);
}

static void print_header_locked(void)
{
	char version[512];
	char bootargs[512];
	char machine[64];
	char model[64];

	// The very first thing the real verbose boot prints is the kernel banner
	if (sysctl_string("kern.version", version, sizeof(version)) == 0) {
		printf_locked(CAT_HEADER, false, "%s", version);
	}
	if (sysctl_string("kern.bootargs", bootargs, sizeof(bootargs)) == 0) {
		printf_locked(CAT_HEADER, false, "boot-args: %s", bootargs[0] ? bootargs : "(none)");
	}
	sysctl_string("hw.machine", machine, sizeof(machine));
	sysctl_string("hw.model", model, sizeof(model));
	printf_locked(CAT_HEADER, false, "hardware: %s (%s)", machine[0] ? machine : "?", model[0] ? model : "?");
	const char *rootPath = jbinfo(rootPath);
	printf_locked(CAT_HEADER, false, "Dopamine: launchdhook loaded, jbroot=%s", rootPath ? rootPath : "?");
	printf_locked(CAT_HEADER, false, "%s", "");

	// Followed by whatever the kernel logged so far
	poll_kmsg_locked(true);
	printf_locked(CAT_LAUNCHD, false, "%s", "");
}

static void arm_watchdog_locked(void)
{
	uint32_t generation = g.generation;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)BOOTLOG_WATCHDOG_SECONDS * (int64_t)NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		pthread_mutex_lock(&gLock);
		if (g.active && g.generation == generation) {
			stop_locked("watchdog timeout", false);
		}
		pthread_mutex_unlock(&gLock);
	});
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
	g.dirty = true;

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
		printf_locked(CAT_LAUNCHD, true, "launchd[1]: userspace reboot requested (kern.willuserspacereboot)");
		printf_locked(CAT_LAUNCHD, true, "launchd[1]: tearing down userspace...");
	}
	else {
		printf_locked(CAT_LAUNCHD, true, "launchd[1]: userspace reboot: launchd re-executed (pid %d)", getpid());
		// Only the launchd that survives the re-exec needs the safety nets
		arm_watchdog_locked();
		setup_springboard_watch_locked();
	}

	flush_now_locked();
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
		vprintf_locked(CAT_LAUNCHD, true, fmt, ap);
		va_end(ap);
		request_flush_locked();
		persist_locked();
	}
	pthread_mutex_unlock(&gLock);
}

void bootlog_dopamine_printf(const char *fmt, ...)
{
	pthread_mutex_lock(&gLock);
	if (g.active) {
		va_list ap;
		va_start(ap, fmt);
		vprintf_locked(CAT_DOPAMINE, true, fmt, ap);
		va_end(ap);
		request_flush_locked();
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

		char label[256] = "";
		if (!strcmp(path, "/usr/libexec/xpcproxy") && argv && argv[0] && argv[1]) {
			// launchd passes the label as argv[1], with a trailing newline
			strlcpy(label, argv[1], sizeof(label));
			size_t len = strlen(label);
			while (len > 0 && isspace((unsigned char)label[len - 1])) label[--len] = '\0';
		}

		if (label[0]) {
			printf_locked(CAT_LAUNCHD, true, "launchd[1]: spawn %s", label);
		}
		else {
			printf_locked(CAT_LAUNCHD, true, "launchd[1]: exec %s", path);
		}

		// backboardd owns the display from here on, but shows nothing until
		// SpringBoard is done launching. With the bootlog daemon around we keep
		// going until it reports that moment (hard cap as a safety net),
		// without it this is where we have to stop.
		bool backboardd = !strcmp(label, "com.apple.backboardd") || !strcmp(path, "/usr/libexec/backboardd");
		if (backboardd && !g.watcherAvailable) {
			stop_locked(label[0] ? label : path, true);
		}
		else {
			if (backboardd) {
				arm_backboardd_hard_cap_locked();
			}
			request_flush_locked();
			persist_locked();
		}
	}
	pthread_mutex_unlock(&gLock);
}

void bootlog_stop(const char *reason)
{
	pthread_mutex_lock(&gLock);
	stop_locked(reason ? reason : "stop requested", false);
	pthread_mutex_unlock(&gLock);
}
