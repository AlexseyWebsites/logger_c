# Logger Library

This C library provides thread-safe logging capabilities with log rotation based on file size. It supports multiple log levels, timestamped entries, and automatic archiving of old logs.

## Features

- Thread-safe logging using Windows mutexes
- Log levels: DEBUG, INFO, WARN, ERROR, FATAL
- Timestamps for each log entry
- Log rotation when the log file reaches a specified size
- Automatic archiving of old logs with timestamped filenames

## How it works

The library initializes with a specified log file name and maximum file size. It writes log messages with timestamps and severity levels to the log file. When the log file exceeds the maximum size, it is renamed with a timestamp and a new log file is created. All operations are synchronized using a mutex to ensure thread safety.

## Usage

### Initialization

```c
#include "logger.h"

int result = logger_init("application.log", 10 * 1024 * 1024); // 10 MB max size
if (result != 0) {
    // handle error
}
```

### Logging messages

```c
log_message(LOG_LEVEL_INFO, "Application started");
log_message(LOG_LEVEL_ERROR, "Failed to open file: %s", filename);
```

### Closing the logger

```c
logger_close();
```

## Functions

- `logger_init(const char* logFileName, size_t maxFileSize)`: Initializes the logger with a log file name and maximum file size in bytes.
- `log_message(LogLevel level, const char* format, ...)`: Logs a formatted message with the specified severity level.
- `logger_close()`: Closes the log file and releases resources.

## Notes

- Ensure that your application has permissions to create and write to the specified log file.
- The maximum file size defaults to a predefined constant (`LOG_MAX_SIZE`) if not specified.
- The log rotation renames the current log file with a timestamp in the format `YYYY-MM-DD_HH-MM-SS.log`.

## Dependencies

- Windows API functions (`CreateMutex`, `WaitForSingleObject`, `ReleaseMutex`, etc.)

---

*This library is designed for Windows platforms. The build requires gcc.*
