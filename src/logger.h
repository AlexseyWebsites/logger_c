#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <windows.h>
#include <stdbool.h>

// Maximum log file size before rotation (for example, 5MB)
#define LOG_MAX_SIZE (5 * 1024 * 1024)

typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL
} LogLevel;

typedef enum {
    LOGGER_SUCCESS = 0,
    LOGGER_ERROR_FILE_OPEN,
    LOGGER_ERROR_MUTEX,
    LOGGER_ERROR_ROTATION,
    LOGGER_ERROR_INVALID_PARAM,
    LOGGER_ERROR_BUFFER_FULL
} LoggerError;

typedef struct {
    char logFileName[256];
    size_t maxFileSize;
    LogLevel minLevel; // Minimum level for logging
    bool outputToConsole; // Console output
    char LogFormat[64]; // String format
    bool enableColors; // Colors in the console (Windows only)
    char logDirectory[256]; // Directory for logs
} LoggerConfig;

// Main functions
int logger_init(const char* logFileName, size_t maxFileSize);
int logger_init_with_config(const LoggerConfig* cfg);
void logger_close();

// Функции логирования
void log_message(LogLevel level, const char* format, ...);
void log_message_with_context(LogLevel level, const char* file, int line, const char* function, const char* format, ...);

// Fast logging functions
void log_debug(const char* format, ...);
void log_info(const char* format, ...);
void log_warn(const char* format, ...);
void log_error(const char* format, ...);
void log_fatal(const char* format, ...);

// Utilities
void set_log_level(LogLevel minLevel);
void set_console_output(bool enabled);
LoggerError get_last_error(void);
const char* logger_strerror(LoggerError error);

// Macros for automatically adding context
#define LOG(level, ...) log_message_with_context(level, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOG_DEBUG(...) LOG(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...) LOG(LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_WARN(...) LOG(LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_ERROR(...) LOG(LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_FATAL(...) LOG(LOG_LEVEL_FATAL, __VA_ARGS__)

// Internal functions (for testing)
int rotate_log_if_needed(void);

#endif // LOGGER_H
