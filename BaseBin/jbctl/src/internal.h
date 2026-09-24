int jbctl_handle_internal(const char *command, int argc, char* argv[]);
#include <stdint.h>
// mach_continuous_time() at jbctl's constructor and at the top of main(), for the bootlog_watch trace
extern uint64_t gJbctlConstructorTime;
extern uint64_t gJbctlMainTime;
