#ifndef _LOGGING2_H
#define _LOGGING2_H

#include <time.h>

extern short DebugLevel;
extern char *DebugFile;

int log_disp_curses(time_t stamp, const char *file, int line, const char *function, int level, ...);

void mutt_log_start(void);
void mutt_log_stop(void);
int mutt_log_set_level(int level);
int mutt_log_set_file(const char *file);

#endif /* _LOGGING2_H */
