#ifndef INFO_H
#define INFO_H
#include <stdbool.h>
struct system_info { struct { char *rootPath; } jailbreakInfo; struct { bool markAppsAsDebugged; double jetsamMultiplier; bool verboseBootEnabled; } jailbreakSettings; };
extern struct system_info gSystemInfo;
#define jbinfo(name) (gSystemInfo.jailbreakInfo.name)
#define jbsetting(name) (gSystemInfo.jailbreakSettings.name)
#endif
