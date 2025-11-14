#include "logger.h"
#include <io.h>  // For _access()
#include <process.h> // For _beginthreadex

static FILE* log_file = NULL;
static char log_filename[256] = {0};
static char current_log_date[16] = {0}; // For daily rotation (YYYY-MM-DD)
static size_t max_file_size = LOG_MAX_SIZE;
static HANDLE hMutex;
static LoggerConfig config = {0};
static LoggerError last_error = LOGGER_SUCCESS;
static char log_buffer[4096]; // Buffer for message formatting

// Asynchronous logging structures
typedef struct {
    LogLevel level;
    char message[1024];
    char file[64];
    int line;
    char function[64];
    char module[64];
    bool has_context;
    bool has_module;
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

// Module filtering
static LogModule modules[LOG_MAX_MODULES];
static size_t module_count = 0;

// Console colors (Windows)
#define COLOR_DEBUG  7  // White
#define COLOR_INFO   10 // Green
#define COLOR_WARN   14 // Yellow
#define COLOR_ERROR  12 // Red
#define COLOR_FATAL  64 // Red on red background

// Internal function declarations
static unsigned __stdcall async_logger_thread(void* param);
static bool should_rotate_by_time(void);
static void update_current_date(void);
static bool is_module_enabled(const char* module, LogLevel level);
static int find_module_index(const char* module_name);

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

static void update_current_date(void) {
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_s(&tm_now, &now);
    strftime(current_log_date, sizeof(current_log_date), "%Y-%m-%d", &tm_now);
}

static bool should_rotate_by_time(void) {
    if (config.rotationType == ROTATION_SIZE) {
        return false;
    }
    
    char today[16];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_s(&tm_now, &now);
    
    if (config.rotationType == ROTATION_DAILY) {
        strftime(today, sizeof(today), "%Y-%m-%d", &tm_now);
    } else if (config.rotationType == ROTATION_HOURLY) {
        strftime(today, sizeof(today), "%Y-%m-%d-%H", &tm_now);
    } else if (config.rotationType == ROTATION_WEEKLY) {
        // Weekly rotation - use year-week number
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
        // If module not registered, use global level
        return level >= config.minLevel;
    }
    
    return modules[index].enabled && level >= modules[index].minLevel;
}

// Enhanced rotation function with time-based support
int rotate_log_if_needed(void) {
    if (!log_file) {
        last_error = LOGGER_ERROR_FILE_OPEN;
        return -1;
    }

    bool should_rotate = false;
    char rotation_reason[64] = "size";
    
    // Check size-based rotation
    if (config.rotationType == ROTATION_SIZE) {
        fseek(log_file, 0, SEEK_END);
        long size = ftell(log_file);
        if (size < 0) {
            last_error = LOGGER_ERROR_ROTATION;
            return -1;
        }
        should_rotate = ((size_t)size >= max_file_size);
    }
    // Check time-based rotation
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
        
        // Update current date for time-based rotation
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

// Fixed asynchronous logger thread function
static unsigned __stdcall async_logger_thread(void* param) {
    (void)param; // Unused parameter
    
    HANDLE wait_handles[2] = { hQueueSemaphore, hShutdownEvent };
    
    while (async_thread_running) {
        DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, 1000);
        
        if (wait_result == WAIT_OBJECT_0) {
            // Process messages from queue
            WaitForSingleObject(hMutex, INFINITE);
            
            while (async_queue_size > 0) {
                LogEntry* entry = &async_queue[async_queue_head];
                
                // Check rotation before writing
                rotate_log_if_needed();
                
                // Write to file - FIXED: use the pre-formatted message directly
                fprintf(log_file, "%s", entry->message);
                fflush(log_file);
                
                // Write to console if enabled - FIXED: use the pre-formatted message
                if (config.outputToConsole) {
                    set_console_color(get_level_color(entry->level));
                    printf("%s", entry->message);
                    reset_console_color();
                }
                
                // Move to next message
                async_queue_head = (async_queue_head + 1) % async_queue_capacity;
                async_queue_size--;
            }
            
            ReleaseMutex(hMutex);
        }
        else if (wait_result == WAIT_OBJECT_0 + 1) {
            // Shutdown event signaled
            break;
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
    config.asyncMode = false;
    config.asyncQueueSize = LOG_MAX_ASYNC_QUEUE;
    config.rotationType = ROTATION_SIZE;
    config.maxBackupFiles = 10;
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

    // Initialize current date for time-based rotation
    update_current_date();

    // Initialize asynchronous logging if enabled
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

    // Open log file
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
    // Signal shutdown for async thread
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
    module_count = 0;
}

// Fixed function to add message to async queue
static bool add_to_async_queue(LogLevel level, const char* formatted_message, 
                              const char* file, int line, const char* function,
                              const char* module) {
    if (!async_thread_running || async_queue_size >= async_queue_capacity) {
        return false;
    }
    
    WaitForSingleObject(hMutex, INFINITE);
    
    if (async_queue_size >= async_queue_capacity) {
        ReleaseMutex(hMutex);
        return false;
    }
    
    LogEntry* entry = &async_queue[async_queue_tail];
    entry->level = level;
    
    // FIXED: Store the complete formatted message directly
    strncpy(entry->message, formatted_message, sizeof(entry->message) - 1);
    entry->message[sizeof(entry->message) - 1] = '\0';
    
    // Store context info for potential future use
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
    
    ReleaseMutex(hMutex);
    
    // Signal the async thread that there's work to do
    ReleaseSemaphore(hQueueSemaphore, 1, NULL);
    
    return true;
}

void log_message(LogLevel level, const char* format, ...) {
    if (!log_file || level < config.minLevel) return;

    // Create timestamp and format message
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    va_end(args);

    char final_message[1024];
    const char* level_str = level_to_string(level);
    snprintf(final_message, sizeof(final_message), "[%s] [%s] %s", 
             timestamp, level_str, log_buffer);

    // Use async queue if enabled
    if (config.asyncMode) {
        if (add_to_async_queue(level, final_message, NULL, 0, NULL, NULL)) {
            return;
        }
        // Fall back to synchronous if queue is full
    }

    // Synchronous logging
    WaitForSingleObject(hMutex, INFINITE);

    if (rotate_log_if_needed() != 0) {
        ReleaseMutex(hMutex);
        return;
    }

    fprintf(log_file, "%s", final_message);
    fflush(log_file);

    if (config.outputToConsole) {
        set_console_color(get_level_color(level));
        printf("%s", final_message);
        reset_console_color();
    }

    ReleaseMutex(hMutex);
}

void log_message_with_context(LogLevel level, const char* file, int line, const char* function, const char* format, ...) {
    if (!log_file || level < config.minLevel) return;

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

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
    snprintf(final_message, sizeof(final_message), "[%s] [%s] [%s:%d:%s] %s", 
             timestamp, level_str, filename, line, function, log_buffer);

    // Use async queue if enabled
    if (config.asyncMode) {
        if (add_to_async_queue(level, final_message, filename, line, function, NULL)) {
            return;
        }
        // Fall back to synchronous if queue is full
    }

    // Synchronous logging
    WaitForSingleObject(hMutex, INFINITE);

    if (rotate_log_if_needed() != 0) {
        ReleaseMutex(hMutex);
        return;
    }

    fprintf(log_file, "%s", final_message);
    fflush(log_file);

    if (config.outputToConsole) {
        set_console_color(get_level_color(level));
        printf("%s", final_message);
        reset_console_color();
    }

    ReleaseMutex(hMutex);
}

void log_message_module(LogLevel level, const char* module, const char* format, ...) {
    if (!log_file || !is_module_enabled(module, level)) return;

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    va_end(args);

    char final_message[1024];
    const char* level_str = level_to_string(level);
    snprintf(final_message, sizeof(final_message), "[%s] [%s] [%s] %s", 
             timestamp, level_str, module, log_buffer);

    // Use async queue if enabled
    if (config.asyncMode) {
        if (add_to_async_queue(level, final_message, NULL, 0, NULL, module)) {
            return;
        }
        // Fall back to synchronous if queue is full
    }

    // Synchronous logging
    WaitForSingleObject(hMutex, INFINITE);

    if (rotate_log_if_needed() != 0) {
        ReleaseMutex(hMutex);
        return;
    }

    fprintf(log_file, "%s", final_message);
    fflush(log_file);

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

// Module-based convenience functions
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
        default: return "Unknown error";
    }
}

// Module management
int logger_register_module(const char* module_name, LogLevel minLevel) {
    if (module_count >= LOG_MAX_MODULES) {
        return -1;
    }
    
    // Check if module already exists
    int index = find_module_index(module_name);
    if (index != -1) {
        modules[index].minLevel = minLevel;
        modules[index].enabled = true;
        return index;
    }
    
    // Register new module
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

// Asynchronous control
void logger_flush(void) {
    if (!config.asyncMode) return;
    
    // Wait for async queue to empty
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

// Time-based rotation control
void logger_force_rotation(void) {
    WaitForSingleObject(hMutex, INFINITE);
    
    if (log_file) {
        fclose(log_file);
        
        // Generate archive name with "manual" reason
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
    
    ReleaseMutex(hMutex);
}
