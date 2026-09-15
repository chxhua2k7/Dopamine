#ifndef BOOTLOG_H
#define BOOTLOG_H

#include <stdbool.h>
#include <stdint.h>

// "Verbose boot" replacement for the static boot logo.
//
// Instead of blitting bootlogo.jp2 to the framebuffer during the userspace
// reboot, this renders a scrolling text console straight into the framebuffer
// using an embedded 8x16 bitmap font (no CoreText / UIKit involved, this runs
// inside launchd). It shows real information: the kernel version banner, the
// tail of the kernel message buffer (dmesg) and every process launchd spawns
// until backboardd comes up and takes the display back.
//
// Lines are colored by what they are (timestamps, kernel messages, launchd
// events, service labels, Dopamine messages, errors / warnings).
//
// Redraws are coalesced (at most one framebuffer swap every few dozen ms) and
// the log stops itself when backboardd (or SpringBoard) is spawned, or after a
// watchdog timeout, so it can never keep painting over the home screen.
//
// The full log of the last userspace reboot is written to
// /var/mobile/Library/Logs/Dopamine/bootlog.txt when the log stops.
//
// All functions are thread safe and become no-ops when the log is not active,
// so callers can invoke them unconditionally.
//
// NOTE: Anything that touches IOSurface / IOMobileFramebuffer has to run with
// asl disabled (see exec_with_asl_disabled in main.m), otherwise os_log gets
// initialized inside launchd and _os_log_simple_reinit_4launchd asserts later.
// The functions below take care of that themselves.

// Start the boot log: takes over the framebuffer and prints the header (kernel
// version, boot-args, dmesg tail).
//
// `beforeUserspaceReboot` should be true when called from the old launchd
// (kern.willuserspacereboot). In that mode every line is also persisted to
// disk, so the new launchd (which calls this with false after the re-exec) can
// restore the screen contents instead of starting over.
//
// Returns 0 on success, -1 if the framebuffer could not be acquired.
int bootlog_start(bool beforeUserspaceReboot);

// Whether the log is currently active (started and not yet stopped).
bool bootlog_is_active(void);

// Append one launchd line (printf style). Long lines wrap like a terminal
// would. Lines are prefixed with the current uptime (same clock and format as
// the kernel's message buffer).
void bootlog_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Same, but the line is attributed to Dopamine itself (different color).
void bootlog_dopamine_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Log a process launch. `path` is the executable that was spawned, `argv` is
// its argument vector (used to extract the launchd label from xpcproxy).
// Also pulls any new kernel messages onto the screen first, and stops the log
// by itself when the spawned process is backboardd or SpringBoard.
void bootlog_spawn_event(const char *path, char *const argv[]);

// Stop the log, release the framebuffer and write the log file.
// `reason` is recorded as the last line of the log file (may be NULL).
void bootlog_stop(const char *reason);

#endif // BOOTLOG_H
