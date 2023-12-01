#ifndef LOG_H_INCLUDED
#define LOG_H_INCLUDED

#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include <string>

static const size_t LOG_LINE_BYTES_MAX = 2048;

enum LOG_LEVEL : int {
    LOG_LEVEL_INVALID,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_WARNING,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG
};

static const char* g_log_level_prefix[] = { // NOTE: Must be synced with LOG_LEVEL
    NULL,
    "ERROR",
    "WARNING",
    "INFO",
    "DEBUG"
};

static LOG_LEVEL g_log_level = LOG_LEVEL_DEBUG;
static FILE* g_log_descriptor = stdout; // Default to stdout until the FILE descriptor has been opened.
static pthread_mutex_t g_log_lock;

int log_init(const char* logfile_path) {
    pthread_mutex_init(&g_log_lock, NULL);
    g_log_descriptor = fopen(logfile_path, "a");
    if(g_log_descriptor == NULL) {
        fprintf(stderr, "Failed to open log file \"%s\". Error %d: %s\n", logfile_path, errno, strerror(errno));
        return 0;
    }
    return 1;
}

void log_close() {
    fclose(g_log_descriptor);
    pthread_mutex_destroy(&g_log_lock);
}

void log(LOG_LEVEL level, const char *fmt, ...) {
    if(level > g_log_level) return;

    // Prepare the variable arguments
    va_list args;
    va_start(args, fmt);

    // Create the datetime string
    time_t rawtime;
    time(&rawtime);
    tm* time_info = localtime(&rawtime);
    const char* datetime_fmt = "%Y-%m-%d %H:%M:%S";
    char datetime_str[strlen("xxxx-xx-xx xx:xx:xx")+1];
    strftime(datetime_str, sizeof(datetime_str), datetime_fmt, time_info);

    // Create a new fmt buffer with the datetime and log level.
    char buf[LOG_LINE_BYTES_MAX];
    snprintf(buf, LOG_LINE_BYTES_MAX, "%s %s: %s\n", datetime_str, g_log_level_prefix[level], fmt);

    pthread_mutex_lock(&g_log_lock);
    vfprintf(g_log_descriptor, buf, args);
    fflush(g_log_descriptor);
    pthread_mutex_unlock(&g_log_lock);
    va_end(args);
}

void log(LOG_LEVEL level, const std::string& str) {
	if(level > g_log_level) return;
	log(level, str.c_str());
}

#endif // LOG_H_INCLUDED
