#include "logger.h"

static FILE* log_file = NULL;
static char log_filename[256] = {0};
static size_t max_file_size = LOG_MAX_SIZE;
static HANDLE hMutex;

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

// Implementation of the rotation function
int rotate_log_if_needed(void) {
    // Checking the file size
    fseek(log_file, 0, SEEK_END);
    long size = ftell(log_file);
    if (size < 0) return -1;
    if ((size_t)size >= max_file_size) {
        // Closing the current file
        fclose(log_file);

        // We create an archive name with the current date and time
        char archive_name[300];
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_s(&tm_now, &now);
        strftime(archive_name, sizeof(archive_name), "%Y-%m-%d_%H-%M-%S.log", &tm_now);

        // Moving the current log to the archive
        rename(log_filename, archive_name);

        // Opening a new log file
        log_file = fopen(log_filename, "a");
        if (!log_file) return -1;
    }
    return 0;
}

int logger_init(const char* logFileName, size_t maxFileSize) {
    if (!logFileName) return -1;
    strncpy(log_filename, logFileName, sizeof(log_filename) - 1);
    max_file_size = maxFileSize ? maxFileSize : LOG_MAX_SIZE;

    // Creating a mutex
    hMutex = CreateMutex(NULL, FALSE, NULL);
    if (!hMutex) return -1;

    // Opening the log file
    log_file = fopen(log_filename, "a");
    if (!log_file) {
        CloseHandle(hMutex);
        return -1;
    }
    return 0;
}

void logger_close() {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    if (hMutex) {
        CloseHandle(hMutex);
        hMutex = NULL;
    }
}

void log_message(LogLevel level, const char* format, ...) {
    if (!log_file) return;

    // Capturing the mutex
    WaitForSingleObject(hMutex, INFINITE);

    // Checking the need for rotation
    rotate_log_if_needed();

    // Creating a timestamp
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    // Forming a message
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    // Writing it to a file
    fprintf(log_file, "[%s] [%s] %s", timestamp, level_to_string(level), message);
    fflush(log_file);

    // Releasing the mutex
    ReleaseMutex(hMutex);
}
