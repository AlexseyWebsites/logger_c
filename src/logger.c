#include "logger.h"
#include <io.h>  // For _access()

static FILE* log_file = NULL;
static char log_filename[256] = {0};
static size_t max_file_size = LOG_MAX_SIZE;
static HANDLE hMutex;
static LoggerConfig config = {0};
static LoggerError last_error = LOGGER_SUCCESS;
static char log_buffer[4096]; // Buffer for message formatting

// Console colors (Windows)
#define COLOR_DEBUG  7  // White
#define COLOR_INFO   10 // Green
#define COLOR_WARN   14 // Yellow
#define COLOR_ERROR  12 // Red
#define COLOR_FATAL  64 // Red on red background

static void set_console_color(int color) {
    if (config.enableColors) {
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        SetConsoleTextAttribute(hConsole, color);
    }
}

static void reset_console_color() {
    if (config.enableColors) {
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        SetConsoleTextAttribute(hConsole, 7); // Reset to default
    }
}

static void get_timestamp(char* buffer, size_t size) {
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_s(&tm_now, &now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &tm_now);
}

static const char* level_to_string(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

static int get_level_color(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return COLOR_DEBUG;
        case LOG_LEVEL_INFO:  return COLOR_INFO;
        case LOG_LEVEL_WARN:  return COLOR_WARN;
        case LOG_LEVEL_ERROR: return COLOR_ERROR;
        case LOG_LEVEL_FATAL: return COLOR_FATAL;
        default: return 7;
    }
}

static bool ensure_directory_exists(const char* path) {
    DWORD attrib = GetFileAttributesA(path);
    if (attrib == INVALID_FILE_ATTRIBUTES) {
        return CreateDirectoryA(path, NULL) != 0;
    }
    return (attrib & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Enhanced rotation function with safety checks
int rotate_log_if_needed(void) {
    if (!log_file) {
        last_error = LOGGER_ERROR_FILE_OPEN;
        return -1;
    }

    fseek(log_file, 0, SEEK_END);
    long size = ftell(log_file);
    if (size < 0) {
        last_error = LOGGER_ERROR_ROTATION;
        return -1;
    }
    
    if ((size_t)size >= max_file_size) {
        fclose(log_file);

        // Create archive directory if needed
        if (strlen(config.logDirectory) > 0) {
            if (!ensure_directory_exists(config.logDirectory)) {
                last_error = LOGGER_ERROR_ROTATION;
                return -1;
            }
        }

        // Generate unique archive name
        char archive_name[300];
        int counter = 0;
        char base_name[256];
        
        // Extract base filename without extension
        const char* dot = strrchr(log_filename, '.');
        if (dot) {
            size_t len = dot - log_filename;
            strncpy(base_name, log_filename, len);
            base_name[len] = '\0';
        } else {
            strcpy(base_name, log_filename);
        }

        do {
            time_t now = time(NULL);
            struct tm tm_now;
            localtime_s(&tm_now, &now);
            
            if (counter == 0) {
                if (strlen(config.logDirectory) > 0) {
                    snprintf(archive_name, sizeof(archive_name), 
                            "%s/%s_%Y-%m-%d_%H-%M-%S.log", 
                            config.logDirectory, base_name);
                } else {
                    snprintf(archive_name, sizeof(archive_name), 
                            "%s_%Y-%m-%d_%H-%M-%S.log", 
                            base_name);
                }
            } else {
                if (strlen(config.logDirectory) > 0) {
                    snprintf(archive_name, sizeof(archive_name), 
                            "%s/%s_%Y-%m-%d_%H-%M-%S_%d.log", 
                            config.logDirectory, base_name, counter);
                } else {
                    snprintf(archive_name, sizeof(archive_name), 
                            "%s_%Y-%m-%d_%H-%M-%S_%d.log", 
                            base_name, counter);
                }
            }
            
            // Format the date string
            char temp[300];
            strftime(temp, sizeof(temp), archive_name, &tm_now);
            strcpy(archive_name, temp);
            
            counter++;
        } while (_access(archive_name, 0) == 0 && counter < 1000);

        if (counter >= 1000) {
            last_error = LOGGER_ERROR_ROTATION;
            return -1;
        }

        if (rename(log_filename, archive_name) != 0) {
            last_error = LOGGER_ERROR_ROTATION;
            return -1;
        }
        
        log_file = fopen(log_filename, "a");
        if (!log_file) {
            last_error = LOGGER_ERROR_FILE_OPEN;
            return -1;
        }
    }
    return 0;
}

int logger_init(const char* logFileName, size_t maxFileSize) {
    // Initialize default config
    memset(&config, 0, sizeof(config));
    strncpy(config.logFileName, logFileName ? logFileName : "application.log", 
            sizeof(config.logFileName) - 1);
    config.maxFileSize = maxFileSize ? maxFileSize : LOG_MAX_SIZE;
    config.minLevel = LOG_LEVEL_DEBUG;
    config.outputToConsole = false;
    config.enableColors = true;
    strcpy(config.logDirectory, "logs");
    
    return logger_init_with_config(&config);
}

int logger_init_with_config(const LoggerConfig* cfg) {
    if (!cfg) {
        last_error = LOGGER_ERROR_INVALID_PARAM;
        return -1;
    }
    
    memcpy(&config, cfg, sizeof(LoggerConfig));
    
    if (strlen(config.logDirectory) > 0) {
        if (!ensure_directory_exists(config.logDirectory)) {
            last_error = LOGGER_ERROR_FILE_OPEN;
            return -1;
        }
        snprintf(log_filename, sizeof(log_filename), "%s/%s", 
                config.logDirectory, config.logFileName);
    } else {
        strncpy(log_filename, config.logFileName, sizeof(log_filename) - 1);
    }
    
    max_file_size = config.maxFileSize ? config.maxFileSize : LOG_MAX_SIZE;

    // Create mutex for thread safety
    hMutex = CreateMutex(NULL, FALSE, NULL);
    if (!hMutex) {
        last_error = LOGGER_ERROR_MUTEX;
        return -1;
    }

    // Open log file
    log_file = fopen(log_filename, "a");
    if (!log_file) {
        CloseHandle(hMutex);
        last_error = LOGGER_ERROR_FILE_OPEN;
        return -1;
    }
    
    last_error = LOGGER_SUCCESS;
    return 0;
}

void logger_close() {
    WaitForSingleObject(hMutex, INFINITE);
    
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        hMutex = NULL;
    }
    
    memset(&config, 0, sizeof(config));
}

void log_message(LogLevel level, const char* format, ...) {
    if (!log_file || level < config.minLevel) return;

    WaitForSingleObject(hMutex, INFINITE);

    // Check if rotation is needed
    if (rotate_log_if_needed() != 0) {
        ReleaseMutex(hMutex);
        return;
    }

    // Create timestamp
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    // Format the message
    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    va_end(args);

    // Format final output string
    char final_message[1024];
    const char* level_str = level_to_string(level);
    
    // Simple format: [timestamp] [level] message
    snprintf(final_message, sizeof(final_message), "[%s] [%s] %s", 
             timestamp, level_str, log_buffer);

    // Write to file
    fprintf(log_file, "%s", final_message);
    fflush(log_file);

    // Write to console if enabled
    if (config.outputToConsole) {
        set_console_color(get_level_color(level));
        printf("%s", final_message);
        reset_console_color();
    }

    ReleaseMutex(hMutex);
}

void log_message_with_context(LogLevel level, const char* file, int line, const char* function, const char* format, ...) {
    if (!log_file || level < config.minLevel) return;

    WaitForSingleObject(hMutex, INFINITE);

    if (rotate_log_if_needed() != 0) {
        ReleaseMutex(hMutex);
        return;
    }

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    // Format the main message
    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    va_end(args);

    // Extract only filename from full path
    const char* filename = strrchr(file, '\\');
    if (!filename) filename = strrchr(file, '/');
    filename = filename ? filename + 1 : file;

    char final_message[1024];
    const char* level_str = level_to_string(level);
    
    // Format with context: [timestamp] [level] [file:line:function] message
    snprintf(final_message, sizeof(final_message), "[%s] [%s] [%s:%d:%s] %s", 
             timestamp, level_str, filename, line, function, log_buffer);

    // File output
    fprintf(log_file, "%s", final_message);
    fflush(log_file);

    // Console output
    if (config.outputToConsole) {
        set_console_color(get_level_color(level));
        printf("%s", final_message);
        reset_console_color();
    }

    ReleaseMutex(hMutex);
}

// Convenience logging functions
void log_debug(const char* format, ...) {
    if (LOG_LEVEL_DEBUG < config.minLevel) return;
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_message(LOG_LEVEL_DEBUG, "%s", buffer);
}

void log_info(const char* format, ...) {
    if (LOG_LEVEL_INFO < config.minLevel) return;
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_message(LOG_LEVEL_INFO, "%s", buffer);
}

void log_warn(const char* format, ...) {
    if (LOG_LEVEL_WARN < config.minLevel) return;
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_message(LOG_LEVEL_WARN, "%s", buffer);
}

void log_error(const char* format, ...) {
    if (LOG_LEVEL_ERROR < config.minLevel) return;
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_message(LOG_LEVEL_ERROR, "%s", buffer);
}

void log_fatal(const char* format, ...) {
    if (LOG_LEVEL_FATAL < config.minLevel) return;
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_message(LOG_LEVEL_FATAL, "%s", buffer);
}

// Utility functions
void set_log_level(LogLevel minLevel) {
    config.minLevel = minLevel;
}

void set_console_output(bool enabled) {
    config.outputToConsole = enabled;
}

LoggerError get_last_error(void) {
    return last_error;
}

const char* logger_strerror(LoggerError error) {
    switch (error) {
        case LOGGER_SUCCESS: return "Success";
        case LOGGER_ERROR_FILE_OPEN: return "Failed to open log file";
        case LOGGER_ERROR_MUTEX: return "Failed to create mutex";
        case LOGGER_ERROR_ROTATION: return "Log rotation failed";
        case LOGGER_ERROR_INVALID_PARAM: return "Invalid parameter";
        case LOGGER_ERROR_BUFFER_FULL: return "Log buffer full";
        default: return "Unknown error";
    }
}
