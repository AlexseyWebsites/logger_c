# Logger Library

A high-performance, thread-safe logging library for C with advanced features including asynchronous logging, module system, log rotation, and guaranteed message delivery.

## Features

- **Thread-safe logging** with Windows mutexes and deadlock prevention
- **Multiple log levels**: DEBUG, INFO, WARN, ERROR, FATAL
- **Asynchronous logging** with configurable queue size
- **Module-based filtering** for different system components
- **Flexible log rotation**: Size-based, Daily, Hourly, Weekly
- **Guaranteed message delivery** for critical events
- **Emergency fallback system** when disk space is low
- **Performance metrics** and health monitoring
- **Colored console output** (optional)
- **Context-aware logging** (file, line, function)
- **Message validation and sanitization** for security

## Quick Start

### Initialization

```c
#include "logger.h"

// Basic initialization
int result = logger_init("application.log", 10 * 1024 * 1024); // 10 MB max size
if (result != 0) {
    // handle error
}

// Or with advanced configuration
LoggerConfig config = {
    .logFileName = "app.log",
    .maxFileSize = 5 * 1024 * 1024,
    .minLevel = LOG_LEVEL_INFO,
    .outputToConsole = true,
    .enableColors = true,
    .asyncMode = true,
    .asyncQueueSize = 5000,
    .rotationType = ROTATION_DAILY,
    .logDirectory = "logs"
};
logger_init_with_config(&config);
```

### Basic Logging

```c
log_message(LOG_LEVEL_INFO, "Application started");
log_message(LOG_LEVEL_ERROR, "Failed to open file: %s", filename);

// Convenience functions
log_info("User %s logged in", username);
log_warn("Disk space running low: %d%% free", free_percent);
log_error("Database connection failed: %s", error_msg);
```

### Context-Aware Logging

```c
// Using macros for automatic file/line/function context
LOG_INFO("Processing request ID: %d", request_id);
LOG_ERROR("Invalid parameter: %s", param_name);

// Manual context
log_message_with_context(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __FUNCTION__, "Detailed debug info");
```

### Module System

```c
// Register modules
logger_register_module("NETWORK", LOG_LEVEL_DEBUG);
logger_register_module("DATABASE", LOG_LEVEL_INFO);
logger_register_module("SECURITY", LOG_LEVEL_WARN);

// Module-specific logging
log_debug_module("NETWORK", "Packet received: %d bytes", packet_size);
log_error_module("DATABASE", "Query timeout: %s", query);
```

### Guaranteed Delivery

```c
// Critical messages that must be delivered
log_message_guaranteed(LOG_LEVEL_FATAL, "System shutting down: %s", reason);
LOG_FATAL_GUARANTEED("Critical failure in module %s", module_name);
```

## Advanced Usage

### Monitoring and Metrics

```c
// Get performance metrics
LoggerMetrics metrics = logger_get_metrics();
printf("Total messages: %zu\n", metrics.total_messages);
printf("Failed messages: %zu\n", metrics.failed_messages);
printf("Queue overflows: %zu\n", metrics.async_queue_overflows);

// Health check
if (!logger_is_healthy()) {
    printf("Logger is experiencing issues\n");
}
```

### Async Control

```c
// Force flush async queue
logger_flush();

// Check queue status
if (logger_is_async_queue_full()) {
    log_warn("Log queue is full, consider increasing queue size");
}

size_t queue_size = logger_get_async_queue_size();
```

### Emergency Recovery

```c
// Manual recovery attempt
if (!logger_attempt_recovery()) {
    logger_emergency_fallback("System in emergency mode");
}

// Force cleanup of old logs
logger_force_cleanup_old_logs();
```

## Configuration Options

| Option | Description | Default |
|--------|-------------|---------|
| `logFileName` | Main log file name | "application.log" |
| `maxFileSize` | Maximum file size before rotation | 5 MB |
| `minLevel` | Minimum log level to record | DEBUG |
| `outputToConsole` | Enable console output | false |
| `enableColors` | Enable colored console output | true |
| `asyncMode` | Enable asynchronous logging | false |
| `asyncQueueSize` | Size of async queue | 10000 |
| `rotationType` | Log rotation strategy | ROTATION_SIZE |
| `logDirectory` | Directory for log files | "logs" |
| `mutexTimeoutMs` | Mutex timeout for deadlock prevention | 1000 ms |

## Error Handling

```c
LoggerError error = get_last_error();
if (error != LOGGER_SUCCESS) {
    printf("Logger error: %s\n", logger_strerror(error));
}
```

## Build Requirements

- **Windows platform** (uses Windows API)
- **C compiler** (GCC, MSVC, Clang)
- **Standard C library**

## Dependencies

- Windows API functions:
  - `CreateMutex`, `WaitForSingleObject`, `ReleaseMutex`
  - `CreateSemaphore`, `CreateEvent`
  - `CreateThread` (for async mode)
  - File I/O functions

## Performance

- **Throughput**: ~1,300 messages/second in async mode
- **Memory**: Configurable queue size with peak usage monitoring
- **Reliability**: 0 message loss in tested scenarios with proper configuration

## Notes

- Ensure application has write permissions to the log directory
- For production use, enable async mode and configure appropriate queue size
- Monitor disk space and logger health in long-running applications
- Use module system to control verbosity of different system components

## License

Copyright. Alexsey Pipichin

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
