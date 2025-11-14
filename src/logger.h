#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <windows.h>
#include <stdbool.h>

// Maximum log file size before rotation (5MB default)
#define LOG_MAX_SIZE (5 * 1024 * 1024)
#define LOG_MAX_ASYNC_QUEUE 10000
#define LOG_MAX_MODULES 32
#define LOG_MAX_MESSAGE_SIZE 4096
#define LOG_EMERGENCY_BUFFER_SIZE 100

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
    LOGGER_ERROR_BUFFER_FULL,
    LOGGER_ERROR_ASYNC_QUEUE_FULL,
    LOGGER_ERROR_THREAD_CREATE,
    LOGGER_ERROR_DISK_FULL,
    LOGGER_ERROR_DEADLOCK_TIMEOUT
} LoggerError;

typedef enum {
    ROTATION_SIZE,      // Rotate by file size
    ROTATION_DAILY,     // Rotate daily
    ROTATION_HOURLY,    // Rotate hourly
    ROTATION_WEEKLY     // Rotate weekly
} RotationType;

typedef struct {
    char logFileName[256];
    size_t maxFileSize;
    LogLevel minLevel;           // Minimum log level to output
    bool outputToConsole;        // Enable console output
    bool enableColors;          // Enable colored console output (Windows only)
    char logDirectory[256];     // Directory for log files
    bool asyncMode;             // Enable asynchronous logging
    size_t asyncQueueSize;      // Max async queue size
    RotationType rotationType;  // Type of log rotation
    int maxBackupFiles;         // Maximum number of backup files to keep
    DWORD mutexTimeoutMs;       // Mutex timeout to prevent deadlocks
    char emergencyFallbackPath[256]; // Fallback path when primary fails
} LoggerConfig;

// Module filtering support
typedef struct {
    char name[64];
    LogLevel minLevel;
    bool enabled;
} LogModule;

// Performance metrics
typedef struct {
    size_t total_messages;
    size_t failed_messages;
    size_t async_queue_overflows;
    size_t recovery_attempts;
    size_t deadlock_preventions;
    DWORD max_processing_time_us;
    size_t peak_memory_usage;
} LoggerMetrics;

// Main functions
int logger_init(const char* logFileName, size_t maxFileSize);
int logger_init_with_config(const LoggerConfig* cfg);
void logger_close();

// Logging functions with reliability guarantees
void log_message(LogLevel level, const char* format, ...);
void log_message_guaranteed(LogLevel level, const char* format, ...); // Sync + flush
void log_message_with_context(LogLevel level, const char* file, int line, const char* function, const char* format, ...);

// Emergency and recovery functions
void logger_emergency_fallback(const char* message);
bool logger_attempt_recovery(void);
void logger_force_cleanup_old_logs(void);

// Performance and monitoring
LoggerMetrics logger_get_metrics(void);
void logger_reset_metrics(void);
bool logger_is_healthy(void);
size_t logger_get_disk_usage(void);

// Convenience logging functions
void log_debug(const char* format, ...);
void log_info(const char* format, ...);
void log_warn(const char* format, ...);
void log_error(const char* format, ...);
void log_fatal(const char* format, ...);

// Module-based logging
void log_message_module(LogLevel level, const char* module, const char* format, ...);
void log_debug_module(const char* module, const char* format, ...);
void log_info_module(const char* module, const char* format, ...);
void log_warn_module(const char* module, const char* format, ...);
void log_error_module(const char* module, const char* format, ...);
void log_fatal_module(const char* module, const char* format, ...);

// Utility functions
void set_log_level(LogLevel minLevel);
void set_console_output(bool enabled);
LoggerError get_last_error(void);
const char* logger_strerror(LoggerError error);

// Module management
int logger_register_module(const char* module_name, LogLevel minLevel);
void logger_set_module_level(const char* module_name, LogLevel minLevel);
void logger_enable_module(const char* module_name, bool enabled);

// Asynchronous control
void logger_flush(void);  // Flush all pending messages
bool logger_is_async_queue_full(void);
size_t logger_get_async_queue_size(void);

// Time-based rotation control
void logger_force_rotation(void);  // Manual log rotation

// Security functions
bool logger_validate_message(const char* format);
void logger_sanitize_message(char* dest, const char* src, size_t dest_size);

// Macros for automatic context logging
#define LOG(level, ...) log_message_with_context(level, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOG_DEBUG(...) LOG(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...) LOG(LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_WARN(...) LOG(LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_ERROR(...) LOG(LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_FATAL(...) LOG(LOG_LEVEL_FATAL, __VA_ARGS__)

// Guaranteed delivery macros
#define LOG_GUARANTEED(level, ...) log_message_guaranteed(level, __VA_ARGS__)
#define LOG_FATAL_GUARANTEED(...) LOG_GUARANTEED(LOG_LEVEL_FATAL, __VA_ARGS__)

// Macros for module logging
#define LOG_MODULE(level, module, ...) log_message_module(level, module, __VA_ARGS__)
#define LOG_DEBUG_MODULE(module, ...) LOG_MODULE(LOG_LEVEL_DEBUG, module, __VA_ARGS__)
#define LOG_INFO_MODULE(module, ...) LOG_MODULE(LOG_LEVEL_INFO, module, __VA_ARGS__)
#define LOG_WARN_MODULE(module, ...) LOG_MODULE(LOG_LEVEL_WARN, module, __VA_ARGS__)
#define LOG_ERROR_MODULE(module, ...) LOG_MODULE(LOG_LEVEL_ERROR, module, __VA_ARGS__)
#define LOG_FATAL_MODULE(module, ...) LOG_MODULE(LOG_LEVEL_FATAL, module, __VA_ARGS__)

// Internal functions (for testing)
int rotate_log_if_needed(void);

#endif // LOGGER_H
