#ifndef _SYS_CAPABILITY_H
#define _SYS_CAPABILITY_H
#include <sys/types.h>
#include <uapi/caps.h>
/* Capabilitati per proces (kernel/proc/caps.h): CAP_* din <uapi/caps.h>, seturi effective / permitted.
 * capget(0, &st) = procesul curent. capset poate doar reduce: nou permitted in vechiul permitted, nou effective in noul permitted */
int capget(int pid, struct cap_state *st);
int capset(const struct cap_state *st);
#endif
