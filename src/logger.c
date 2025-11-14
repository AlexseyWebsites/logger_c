#include "logger.h"
#include <io.h>
#include <process.h>

static FILE* log_file = NULL;
static char log_filename[256] = {0};
static char current_log_date[16] = {0};
static size_t max_file_size = LOG_MAX_SIZE;
static HANDLE hMutex;
static LoggerConfig config = {0};
static LoggerError last_error = LOGGER_SUCCESS;
static char log_buffer[LOG_MAX_MESSAGE_SIZE];
static LoggerMetrics metrics = {0};

// Async structures
typedef struct {
    LogLevel level;
    char message[LOG_MAX_MESSAGE_SIZE];
    char file[64];
    int line;
    char function[64];
    char module[64];
    bool has_context;
    bool has_module;
    bool guaranteed;
} LogEntry;

static LogEntry* async_queue = NULL;
static size_t async_queue_capacity = 0;
static size_t async_queue_size = 0;
static size_t async_queue_head = 0;
static size_t async_queue_tail = 0;
static HANDLE hAsyncThread = NULL;
static HANDLE hQueueSemaphore = NULL;
static HANDLE hShutdownEvent = NULL;
static volatile bool async_thread_running = false;

// Emergency system
static char emergency_buffer[LOG_EMERGENCY_BUFFER_SIZE][256];
static size_t emergency_buffer_head = 0;
static size_t emergency_buffer_tail = 0;
static bool emergency_mode = false;

// Module system
static LogModule modules[LOG_MAX_MODULES];
static size_t module_count = 0;

// Performance
static LARGE_INTEGER performance_frequency;
static bool performance_counter_initialized = false;

// Colors
#define COLOR_DEBUG  7
#define COLOR_INFO   10
#define COLOR_WARN   14
#define COLOR_ERROR  12
#define COLOR_FATAL  64

// Internal functions
static unsigned __stdcall async_logger_thread(void* param);
static bool should_rotate_by_time(void);
static void update_current_date(void);
static bool is_module_enabled(const char* module, LogLevel level);
static int find_module_index(const char* module_name);
static bool try_lock_mutex_with_timeout(void);
static void unlock_mutex(void);
static DWORD get_current_time_us(void);
static void update_performance_metrics(DWORD start_time, size_t message_size);
static bool attempt_disk_recovery(void);
static void emergency_fallback_write(const char* message);
static bool is_disk_space_low(void);
static void init_performance_counter(void);

static void init_performance_counter(void) {
    if (!performance_counter_initialized) {
        QueryPerformanceFrequency(&performance_frequency);
        performance_counter_initialized = true;
    }
}

static DWORD get_current_time_us(void) {
    LARGE_INTEGER current_time;
    QueryPerformanceCounter(&current_time);
    return (DWORD)((current_time.QuadPart * 1000000) / performance_frequency.QuadPart);
}

static void update_performance_metrics(DWORD start_time, size_t message_size) {
    DWORD end_time = get_current_time_us();
    DWORD processing_time = end_time - start_time;
    
    metrics.total_messages++;
    if (processing_time > metrics.max_processing_time_us) {
        metrics.max_processing_time_us = processing_time;
    }
    
    size_t current_usage = async_queue_size * sizeof(LogEntry);
    if (current_usage > metrics.peak_memory_usage) {
        metrics.peak_memory_usage = current_usage;
    }
}

static void set_console_color(int color) {
    if (config.enableColors) {
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        SetConsoleTextAttribute(hConsole, color);
    }
}

static void reset_console_color() {
    if (config.enableColors) {
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        SetConsoleTextAttribute(hConsole, 7);
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

static bool try_lock_mutex_with_timeout(void) {
    if (!hMutex) return false;
    
    DWORD timeout = config.mutexTimeoutMs ? config.mutexTimeoutMs : LOG_MUTEX_TIMEOUT_MS;
    DWORD result = WaitForSingleObject(hMutex, timeout);
    
    if (result == WAIT_TIMEOUT) {
        metrics.deadlock_preventions++;
        last_error = LOGGER_ERROR_DEADLOCK_TIMEOUT;
        return false;
    }
    
    return result == WAIT_OBJECT_0;
}

static void unlock_mutex(void) {
    if (hMutex) {
        ReleaseMutex(hMutex);
    }
}

static bool is_disk_space_low(void) {
    ULARGE_INTEGER free_bytes_available;
    
    if (GetDiskFreeSpaceExA(config.logDirectory, &free_bytes_available, NULL, NULL)) {
        return free_bytes_available.QuadPart < (100 * 1024 * 1024);
    }
    
    return true;
}

static bool attempt_disk_recovery(void) {
    metrics.recovery_attempts++;
    
    // Try to free space
    logger_force_cleanup_old_logs();
    
    // Reopen file
    if (log_file) {
        fclose(log_file);
    }
    
    log_file = fopen(log_filename, "a");
    if (log_file) {
        emergency_mode = false;
        return true;
    }
    
    emergency_mode = true;
    return false;
}

static void emergency_fallback_write(const char* message) {
    strncpy(emergency_buffer[emergency_buffer_tail], message, 
            sizeof(emergency_buffer[0]) - 1);
    emergency_buffer[emergency_buffer_tail][sizeof(emergency_buffer[0]) - 1] = '\0';
    
    emergency_buffer_tail = (emergency_buffer_tail + 1) % LOG_EMERGENCY_BUFFER_SIZE;
    if (emergency_buffer_tail == emergency_buffer_head) {
        emergency_buffer_head = (emergency_buffer_head + 1) % LOG_EMERGENCY_BUFFER_SIZE;
    }
    
    fprintf(stderr, "EMERGENCY: %s", message);
}

static void update_current_date(void) {
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_s(&tm_now, &now);
    strftime(current_log_date, sizeof(current_log_date), "%Y-%m-%d", &tm_now);
}

static bool should_rotate_by_time(void) {
    if (config.rotationType == ROTATION_SIZE) return false;
    
    char today[16];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_s(&tm_now, &now);
    
    if (config.rotationType == ROTATION_DAILY) {
        strftime(today, sizeof(today), "%Y-%m-%d", &tm_now);
    } else if (config.rotationType == ROTATION_HOURLY) {
        strftime(today, sizeof(today), "%Y-%m-%d-%H", &tm_now);
    } else if (config.rotationType == ROTATION_WEEKLY) {
        strftime(today, sizeof(today), "%Y-W%U", &tm_now);
    }
    
    return strcmp(current_log_date, today) != 0;
}

static int find_module_index(const char* module_name) {
    for (size_t i = 0; i < module_count; i++) {
        if (strcmp(modules[i].name, module_name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool is_module_enabled(const char* module, LogLevel level) {
    if (module == NULL || strlen(module) == 0) {
        return level >= config.minLevel;
    }
    
    int index = find_module_index(module);
    if (index == -1) {
        return level >= config.minLevel;
    }
    
    return modules[index].enabled && level >= modules[index].minLevel;
}

int rotate_log_if_needed(void) {
    if (!log_file) {
        last_error = LOGGER_ERROR_FILE_OPEN;
        return -1;
    }

    if (is_disk_space_low()) {
        if (!attempt_disk_recovery()) {
            last_error = LOGGER_ERROR_DISK_FULL;
            return -1;
        }
    }

    bool should_rotate = false;
    char rotation_reason[64] = "size";
    
    if (config.rotationType == ROTATION_SIZE) {
        fseek(log_file, 0, SEEK_END);
        long size = ftell(log_file);
        if (size < 0) {
            last_error = LOGGER_ERROR_ROTATION;
            return -1;
        }
        should_rotate = ((size_t)size >= max_file_size);
    }
    else if (should_rotate_by_time()) {
        should_rotate = true;
        if (config.rotationType == ROTATION_DAILY) {
            strcpy(rotation_reason, "daily");
        } else if (config.rotationType == ROTATION_HOURLY) {
            strcpy(rotation_reason, "hourly");
        } else if (config.rotationType == ROTATION_WEEKLY) {
            strcpy(rotation_reason, "weekly");
        }
    }
    
    if (should_rotate) {
        fclose(log_file);

        if (strlen(config.logDirectory) > 0) {
            if (!ensure_directory_exists(config.logDirectory)) {
                last_error = LOGGER_ERROR_ROTATION;
                return -1;
            }
        }

        char archive_name[300];
        int counter = 0;
        char base_name[256];
        
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
                            "%s/%s_%s_%Y-%m-%d_%H-%M-%S.log", 
                            config.logDirectory, base_name, rotation_reason);
                } else {
                    snprintf(archive_name, sizeof(archive_name), 
                            "%s_%s_%Y-%m-%d_%H-%M-%S.log", 
                            base_name, rotation_reason);
                }
            } else {
                if (strlen(config.logDirectory) > 0) {
                    snprintf(archive_name, sizeof(archive_name), 
                            "%s/%s_%s_%Y-%m-%d_%H-%M-%S_%d.log", 
                            config.logDirectory, base_name, rotation_reason, counter);
                } else {
                    snprintf(archive_name, sizeof(archive_name), 
                            "%s_%s_%Y-%m-%d_%H-%M-%S_%d.log", 
                            base_name, rotation_reason, counter);
                }
            }
            
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
        
        if (config.rotationType != ROTATION_SIZE) {
            update_current_date();
        }
        
        log_file = fopen(log_filename, "a");
        if (!log_file) {
            last_error = LOGGER_ERROR_FILE_OPEN;
            return -1;
        }
    }
    return 0;
}

static unsigned __stdcall async_logger_thread(void* param) {
    (void)param;
    
    HANDLE wait_handles[2] = { hQueueSemaphore, hShutdownEvent };
    
    while (async_thread_running) {
        DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, 100);
        
        if (wait_result == WAIT_OBJECT_0) {
            if (!try_lock_mutex_with_timeout()) {
                continue;
            }
            
            while (async_queue_size > 0) {
                LogEntry* entry = &async_queue[async_queue_head];
                DWORD start_time = get_current_time_us();
                
                if (entry->guaranteed) {
                    if (!emergency_mode) {
                        rotate_log_if_needed();
                        fprintf(log_file, "%s", entry->message);
                        fflush(log_file);
                    } else {
                        emergency_fallback_write(entry->message);
                    }
                } else {
                    if (!emergency_mode && rotate_log_if_needed() == 0) {
                        fprintf(log_file, "%s", entry->message);
                    }
                }
                
                if (config.outputToConsole && !emergency_mode) {
                    set_console_color(get_level_color(entry->level));
                    printf("%s", entry->message);
                    reset_console_color();
                }
                
                async_queue_head = (async_queue_head + 1) % async_queue_capacity;
                async_queue_size--;
                
                update_performance_metrics(start_time, strlen(entry->message));
            }
            
            unlock_mutex();
        }
        else if (wait_result == WAIT_OBJECT_0 + 1) {
            break;
        }
        
        if (log_file) {
            fflush(log_file);
        }
    }
    
    if (log_file) {
        fflush(log_file);
    }
    
    return 0;
}

bool logger_validate_message(const char* format) {
    if (!format) return false;
    
    const char* p = format;
    int percent_count = 0;
    while (*p) {
        if (*p == '%') {
            percent_count++;
            p++;
            if (*p != '%' && *p != 's' && *p != 'd' && *p != 'f' && 
                *p != 'c' && *p != 'x' && *p != 'p') {
                return false;
            }
        }
        p++;
    }
    
    return strlen(format) < LOG_MAX_MESSAGE_SIZE - 100;
}

void logger_sanitize_message(char* dest, const char* src, size_t dest_size) {
    if (!dest || !src || dest_size == 0) return;
    
    size_t i = 0;
    while (src[i] != '\0' && i < dest_size - 1) {
        if (src[i] >= 32 || src[i] == '\n' || src[i] == '\t') {
            dest[i] = src[i];
        } else {
            dest[i] = '?';
        }
        i++;
    }
    dest[i] = '\0';
}

static bool add_to_async_queue(LogLevel level, const char* formatted_message, 
                              const char* file, int line, const char* function,
                              const char* module, bool guaranteed) {
    if (!async_thread_running || async_queue_size >= async_queue_capacity) {
        if (guaranteed) {
            return false;
        }
        metrics.async_queue_overflows++;
        return false;
    }
    
    if (!try_lock_mutex_with_timeout()) {
        return false;
    }
    
    if (async_queue_size >= async_queue_capacity) {
        unlock_mutex();
        metrics.async_queue_overflows++;
        return false;
    }
    
    LogEntry* entry = &async_queue[async_queue_tail];
    entry->level = level;
    entry->guaranteed = guaranteed;
    
    strncpy(entry->message, formatted_message, sizeof(entry->message) - 1);
    entry->message[sizeof(entry->message) - 1] = '\0';
    
    if (file && function) {
        entry->has_context = true;
        strncpy(entry->file, file, sizeof(entry->file) - 1);
        entry->file[sizeof(entry->file) - 1] = '\0';
        entry->line = line;
        strncpy(entry->function, function, sizeof(entry->function) - 1);
        entry->function[sizeof(entry->function) - 1] = '\0';
    } else {
        entry->has_context = false;
    }
    
    if (module) {
        entry->has_module = true;
        strncpy(entry->module, module, sizeof(entry->module) - 1);
        entry->module[sizeof(entry->module) - 1] = '\0';
    } else {
        entry->has_module = false;
    }
    
    async_queue_tail = (async_queue_tail + 1) % async_queue_capacity;
    async_queue_size++;
    
    unlock_mutex();
    
    ReleaseSemaphore(hQueueSemaphore, 1, NULL);
    
    return true;
}

int logger_init(const char* logFileName, size_t maxFileSize) {
    memset(&config, 0, sizeof(config));
    strncpy(config.logFileName, logFileName ? logFileName : "application.log", 
            sizeof(config.logFileName) - 1);
    config.maxFileSize = maxFileSize ? maxFileSize : LOG_MAX_SIZE;
    config.minLevel = LOG_LEVEL_DEBUG;
    config.outputToConsole = false;
    config.enableColors = true;
    config.asyncMode = false;
    config.asyncQueueSize = LOG_MAX_ASYNC_QUEUE;
    config.rotationType = ROTATION_SIZE;
    config.maxBackupFiles = 10;
    config.mutexTimeoutMs = LOG_MUTEX_TIMEOUT_MS;
    strcpy(config.logDirectory, "logs");
    strcpy(config.emergencyFallbackPath, "C:\\temp\\logs");
    
    return logger_init_with_config(&config);
}

int logger_init_with_config(const LoggerConfig* cfg) {
    if (!cfg) {
        last_error = LOGGER_ERROR_INVALID_PARAM;
        return -1;
    }
    
    memcpy(&config, cfg, sizeof(LoggerConfig));
    init_performance_counter();
    
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

    hMutex = CreateMutex(NULL, FALSE, NULL);
    if (!hMutex) {
        last_error = LOGGER_ERROR_MUTEX;
        return -1;
    }

    update_current_date();

    if (config.asyncMode) {
        async_queue_capacity = config.asyncQueueSize ? config.asyncQueueSize : LOG_MAX_ASYNC_QUEUE;
        async_queue = (LogEntry*)malloc(async_queue_capacity * sizeof(LogEntry));
        if (!async_queue) {
            CloseHandle(hMutex);
            last_error = LOGGER_ERROR_ASYNC_QUEUE_FULL;
            return -1;
        }
        
        hQueueSemaphore = CreateSemaphore(NULL, 0, (LONG)async_queue_capacity, NULL);
        hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        
        if (!hQueueSemaphore || !hShutdownEvent) {
            free(async_queue);
            CloseHandle(hMutex);
            if (hQueueSemaphore) CloseHandle(hQueueSemaphore);
            if (hShutdownEvent) CloseHandle(hShutdownEvent);
            last_error = LOGGER_ERROR_THREAD_CREATE;
            return -1;
        }
        
        async_thread_running = true;
        hAsyncThread = (HANDLE)_beginthreadex(NULL, 0, async_logger_thread, NULL, 0, NULL);
        if (!hAsyncThread) {
            async_thread_running = false;
            free(async_queue);
            CloseHandle(hMutex);
            CloseHandle(hQueueSemaphore);
            CloseHandle(hShutdownEvent);
            last_error = LOGGER_ERROR_THREAD_CREATE;
            return -1;
        }
    }

    log_file = fopen(log_filename, "a");
    if (!log_file) {
        logger_close();
        last_error = LOGGER_ERROR_FILE_OPEN;
        return -1;
    }
    
    last_error = LOGGER_SUCCESS;
    return 0;
}

void logger_close() {
    if (async_thread_running) {
        async_thread_running = false;
        SetEvent(hShutdownEvent);
        
        if (hAsyncThread) {
            WaitForSingleObject(hAsyncThread, 5000);
            CloseHandle(hAsyncThread);
            hAsyncThread = NULL;
        }
        
        if (hQueueSemaphore) {
            CloseHandle(hQueueSemaphore);
            hQueueSemaphore = NULL;
        }
        
        if (hShutdownEvent) {
            CloseHandle(hShutdownEvent);
            hShutdownEvent = NULL;
        }
        
        if (async_queue) {
            free(async_queue);
            async_queue = NULL;
        }
    }
    
    if (!try_lock_mutex_with_timeout()) {
        if (hMutex) {
            CloseHandle(hMutex);
            hMutex = NULL;
        }
    } else {
        if (log_file) {
            fclose(log_file);
            log_file = NULL;
        }
        unlock_mutex();
        
        if (hMutex) {
            CloseHandle(hMutex);
            hMutex = NULL;
        }
    }
    
    memset(&config, 0, sizeof(config));
    module_count = 0;
}

void log_message_guaranteed(LogLevel level, const char* format, ...) {
    if (!logger_validate_message(format)) {
        return;
    }

    DWORD start_time = get_current_time_us();
    char sanitized_format[LOG_MAX_MESSAGE_SIZE];
    logger_sanitize_message(sanitized_format, format, sizeof(sanitized_format));

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer, sizeof(log_buffer), sanitized_format, args);
    va_end(args);

    char final_message[LOG_MAX_MESSAGE_SIZE];
    const char* level_str = level_to_string(level);
    snprintf(final_message, sizeof(final_message), "[%s] [%s] %s", 
             timestamp, level_str, log_buffer);

    if (config.asyncMode) {
        if (add_to_async_queue(level, final_message, NULL, 0, NULL, NULL, true)) {
            update_performance_metrics(start_time, strlen(final_message));
            return;
        }
    }

    if (!try_lock_mutex_with_timeout()) {
        emergency_fallback_write(final_message);
        metrics.failed_messages++;
        return;
    }

    if (emergency_mode || rotate_log_if_needed() != 0) {
        emergency_fallback_write(final_message);
    } else {
        fprintf(log_file, "%s", final_message);
        fflush(log_file);
    }

    if (config.outputToConsole && !emergency_mode) {
        set_console_color(get_level_color(level));
        printf("%s", final_message);
        reset_console_color();
    }

    unlock_mutex();
    update_performance_metrics(start_time, strlen(final_message));
}

void log_message(LogLevel level, const char* format, ...) {
    if (!log_file || level < config.minLevel || !logger_validate_message(format)) {
        return;
    }

    DWORD start_time = get_current_time_us();
    char sanitized_format[LOG_MAX_MESSAGE_SIZE];
    logger_sanitize_message(sanitized_format, format, sizeof(sanitized_format));

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer, sizeof(log_buffer), sanitized_format, args);
    va_end(args);

    char final_message[LOG_MAX_MESSAGE_SIZE];
    const char* level_str = level_to_string(level);
    snprintf(final_message, sizeof(final_message), "[%s] [%s] %s", 
             timestamp, level_str, log_buffer);

    if (config.asyncMode) {
        if (add_to_async_queue(level, final_message, NULL, 0, NULL, NULL, false)) {
            update_performance_metrics(start_time, strlen(final_message));
            return;
        }
    }

    if (!try_lock_mutex_with_timeout()) {
        metrics.failed_messages++;
        return;
    }

    if (emergency_mode || rotate_log_if_needed() != 0) {
        emergency_fallback_write(final_message);
    } else {
        fprintf(log_file, "%s", final_message);
    }

    if (config.outputToConsole && !emergency_mode) {
        set_console_color(get_level_color(level));
        printf("%s", final_message);
        reset_console_color();
    }

    unlock_mutex();
    update_performance_metrics(start_time, strlen(final_message));
}

void log_message_with_context(LogLevel level, const char* file, int line, const char* function, const char* format, ...) {
    if (!log_file || level < config.minLevel || !logger_validate_message(format)) return;

    DWORD start_time = get_current_time_us();
    char sanitized_format[LOG_MAX_MESSAGE_SIZE];
    logger_sanitize_message(sanitized_format, format, sizeof(sanitized_format));

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer, sizeof(log_buffer), sanitized_format, args);
    va_end(args);

    const char* filename = strrchr(file, '\\');
    if (!filename) filename = strrchr(file, '/');
    filename = filename ? filename + 1 : file;

    char final_message[LOG_MAX_MESSAGE_SIZE];
    const char* level_str = level_to_string(level);
    snprintf(final_message, sizeof(final_message), "[%s] [%s] [%s:%d:%s] %s", 
             timestamp, level_str, filename, line, function, log_buffer);

    if (config.asyncMode) {
        if (add_to_async_queue(level, final_message, filename, line, function, NULL, false)) {
            update_performance_metrics(start_time, strlen(final_message));
            return;
        }
    }

    if (!try_lock_mutex_with_timeout()) {
        metrics.failed_messages++;
        return;
    }

    if (emergency_mode || rotate_log_if_needed() != 0) {
        emergency_fallback_write(final_message);
    } else {
        fprintf(log_file, "%s", final_message);
    }

    if (config.outputToConsole && !emergency_mode) {
        set_console_color(get_level_color(level));
        printf("%s", final_message);
        reset_console_color();
    }

    unlock_mutex();
    update_performance_metrics(start_time, strlen(final_message));
}

void log_message_module(LogLevel level, const char* module, const char* format, ...) {
    if (!log_file || !is_module_enabled(module, level) || !logger_validate_message(format)) return;

    DWORD start_time = get_current_time_us();
    char sanitized_format[LOG_MAX_MESSAGE_SIZE];
    logger_sanitize_message(sanitized_format, format, sizeof(sanitized_format));

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer, sizeof(log_buffer), sanitized_format, args);
    va_end(args);

    char final_message[LOG_MAX_MESSAGE_SIZE];
    const char* level_str = level_to_string(level);
    snprintf(final_message, sizeof(final_message), "[%s] [%s] [%s] %s", 
             timestamp, level_str, module, log_buffer);

    if (config.asyncMode) {
        if (add_to_async_queue(level, final_message, NULL, 0, NULL, module, false)) {
            update_performance_metrics(start_time, strlen(final_message));
            return;
        }
    }

    if (!try_lock_mutex_with_timeout()) {
        metrics.failed_messages++;
        return;
    }

    if (emergency_mode || rotate_log_if_needed() != 0) {
        emergency_fallback_write(final_message);
    } else {
        fprintf(log_file, "%s", final_message);
    }

    if (config.outputToConsole && !emergency_mode) {
        set_console_color(get_level_color(level));
        printf("%s", final_message);
        reset_console_color();
    }

    unlock_mutex();
    update_performance_metrics(start_time, strlen(final_message));
}

// Convenience functions
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

// Module convenience functions
void log_debug_module(const char* module, const char* format, ...) {
    if (!is_module_enabled(module, LOG_LEVEL_DEBUG)) return;
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_message_module(LOG_LEVEL_DEBUG, module, "%s", buffer);
}

void log_info_module(const char* module, const char* format, ...) {
    if (!is_module_enabled(module, LOG_LEVEL_INFO)) return;
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_message_module(LOG_LEVEL_INFO, module, "%s", buffer);
}

void log_warn_module(const char* module, const char* format, ...) {
    if (!is_module_enabled(module, LOG_LEVEL_WARN)) return;
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_message_module(LOG_LEVEL_WARN, module, "%s", buffer);
}

void log_error_module(const char* module, const char* format, ...) {
    if (!is_module_enabled(module, LOG_LEVEL_ERROR)) return;
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_message_module(LOG_LEVEL_ERROR, module, "%s", buffer);
}

void log_fatal_module(const char* module, const char* format, ...) {
    if (!is_module_enabled(module, LOG_LEVEL_FATAL)) return;
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log_message_module(LOG_LEVEL_FATAL, module, "%s", buffer);
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
        case LOGGER_ERROR_ASYNC_QUEUE_FULL: return "Async log queue full";
        case LOGGER_ERROR_THREAD_CREATE: return "Failed to create async thread";
        case LOGGER_ERROR_DISK_FULL: return "Disk full";
        case LOGGER_ERROR_DEADLOCK_TIMEOUT: return "Mutex timeout - possible deadlock";
        default: return "Unknown error";
    }
}

// Module management
int logger_register_module(const char* module_name, LogLevel minLevel) {
    if (module_count >= LOG_MAX_MODULES) {
        return -1;
    }
    
    int index = find_module_index(module_name);
    if (index != -1) {
        modules[index].minLevel = minLevel;
        modules[index].enabled = true;
        return index;
    }
    
    strncpy(modules[module_count].name, module_name, sizeof(modules[0].name) - 1);
    modules[module_count].name[sizeof(modules[0].name) - 1] = '\0';
    modules[module_count].minLevel = minLevel;
    modules[module_count].enabled = true;
    
    return (int)module_count++;
}

void logger_set_module_level(const char* module_name, LogLevel minLevel) {
    int index = find_module_index(module_name);
    if (index != -1) {
        modules[index].minLevel = minLevel;
    }
}

void logger_enable_module(const char* module_name, bool enabled) {
    int index = find_module_index(module_name);
    if (index != -1) {
        modules[index].enabled = enabled;
    }
}

// Async control
void logger_flush(void) {
    if (!config.asyncMode) return;
    
    while (async_queue_size > 0) {
        Sleep(10);
    }
}

bool logger_is_async_queue_full(void) {
    return config.asyncMode && async_queue_size >= async_queue_capacity;
}

size_t logger_get_async_queue_size(void) {
    return async_queue_size;
}

// Rotation
void logger_force_rotation(void) {
    if (!try_lock_mutex_with_timeout()) {
        return;
    }
    
    if (log_file) {
        fclose(log_file);
        
        char archive_name[300];
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_s(&tm_now, &now);
        
        char base_name[256];
        const char* dot = strrchr(log_filename, '.');
        if (dot) {
            size_t len = dot - log_filename;
            strncpy(base_name, log_filename, len);
            base_name[len] = '\0';
        } else {
            strcpy(base_name, log_filename);
        }
        
        if (strlen(config.logDirectory) > 0) {
            snprintf(archive_name, sizeof(archive_name), 
                    "%s/%s_manual_%Y-%m-%d_%H-%M-%S.log", 
                    config.logDirectory, base_name);
        } else {
            snprintf(archive_name, sizeof(archive_name), 
                    "%s_manual_%Y-%m-%d_%H-%M-%S.log", 
                    base_name);
        }
        
        char temp[300];
        strftime(temp, sizeof(temp), archive_name, &tm_now);
        
        rename(log_filename, temp);
        update_current_date();
        
        log_file = fopen(log_filename, "a");
    }
    
    unlock_mutex();
}

// Emergency functions
void logger_emergency_fallback(const char* message) {
    emergency_fallback_write(message);
}

bool logger_attempt_recovery(void) {
    return attempt_disk_recovery();
}

void logger_force_cleanup_old_logs(void) {
    // Simplified cleanup - in production would implement file age checking
    printf("Cleanup: Would remove old log files here\n");
}

// Monitoring
LoggerMetrics logger_get_metrics(void) {
    return metrics;
}

void logger_reset_metrics(void) {
    metrics = (LoggerMetrics){0};
}

bool logger_is_healthy(void) {
    return !emergency_mode && 
           async_queue_size < async_queue_capacity * 0.9 &&
           last_error == LOGGER_SUCCESS;
}

size_t logger_get_disk_usage(void) {
    if (!log_file) return 0;
    
    fseek(log_file, 0, SEEK_END);
    long size = ftell(log_file);
    fseek(log_file, 0, SEEK_SET);
    
    return size > 0 ? (size_t)size : 0;
}
