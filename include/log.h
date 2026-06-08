#ifndef DI_LOG_H
#define DI_LOG_H

#include <stdarg.h>

void di_log_open(void);
void di_log_close(void);
void di_logf(const char *fmt, ...);
void di_logv(const char *fmt, va_list args);
const char *di_log_path(void);

#endif
