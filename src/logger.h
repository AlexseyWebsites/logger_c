#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <windows.h>

// Maximum log file size before rotation (for example, 5MB)
#define LOG_MAX_SIZE (5 * 1024 * 1024)

typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL
} LogLevel;

// logFileName: the base name of the log file
// maxFileSize: maximum file size before rotation (in bytes)
int logger_init(const char* logFileName, size_t maxFileSize);

// Freeing up logger resources
void logger_close();

// The main logging function
void log_message(LogLevel level, const char* format, ...);

// Declaring the rotation function
int rotate_log_if_needed(void);

#endif // LOGGER_H
