#include "../src/logger.h"
#include <stdio.h>

int main() {
    // Simple initialization
    printf("Testing basic logger...\n");
    logger_init("test_basic.log", 1024); // 1KB for rotation demo
    
    log_info("Application started with basic logger\n");
    log_debug("Debug message: %s\n", "This is debug info");
    log_warn("Warning: %d files found\n", 5);
    log_error("Error occurred: %s\n", "File not found");
    
    logger_close();
    
    // Advanced initialization with config
    printf("Testing advanced logger...\n");
    LoggerConfig cfg = {
        .logFileName = "test_advanced.log",
        .maxFileSize = 2048, // 2KB
        .minLevel = LOG_LEVEL_INFO,
        .outputToConsole = true,
        .enableColors = true,
        .logDirectory = "logs"
    };
    
    if (logger_init_with_config(&cfg) != 0) {
        printf("Failed to initialize logger: %s\n", logger_strerror(get_last_error()));
        return 1;
    }
    
    // Using macros with context
    LOG_INFO("Application started with advanced config\n");
    LOG_DEBUG("This debug message won't appear due to minLevel\n");
    LOG_WARN("User '%s' performed action '%s'\n", "john_doe", "login");
    LOG_ERROR("Database connection failed: %s\n", "Timeout");
    
    // Generate many messages for rotation testing
    for (int i = 0; i < 50; i++) {
        LOG_INFO("Test message %d for rotation testing\n", i);
    }
    
    logger_close();
    
    printf("All tests completed. Check 'logs' directory.\n");
    return 0;
}
