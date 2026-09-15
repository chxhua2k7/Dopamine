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

// Append one line (printf style). Long lines wrap like a terminal would.
// Lines are prefixed with the current uptime.
void bootlog_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Log a process launch. `path` is the executable that was spawned, `argv` is
// its argument vector (used to extract the launchd label from xpcproxy).
// Also pulls any new kernel messages onto the screen first.
void bootlog_spawn_event(const char *path, char *const argv[]);

// Stop the log and release the framebuffer (call before backboardd starts).
void bootlog_stop(void);

#endif // BOOTLOG_H
