#include "log.h"

#include <stdarg.h>
#include <stddef.h>

void di_log_open(void) {
}

void di_log_close(void) {
}

void di_logv(const char *fmt, va_list args) {
    (void)fmt;
    (void)args;
}

void di_logf(const char *fmt, ...) {
    (void)fmt;
}

const char *di_log_path(void) {
    return NULL;
}
