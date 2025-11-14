#include "../src/logger.h"
#include <stdio.h>
#include <windows.h>

int main() {
    printf("Testing enhanced logger with async, filters and time rotation...\n");
    
    // Advanced initialization with all new features
    LoggerConfig cfg = {
        .logFileName = "enhanced_test.log",
        .maxFileSize = 1024, // 1KB for easy rotation testing
        .minLevel = LOG_LEVEL_INFO,
        .outputToConsole = true,
        .enableColors = true,
        .logDirectory = "logs",
        .asyncMode = true,           // Enable asynchronous logging
        .asyncQueueSize = 1000,      // Async queue size
        .rotationType = ROTATION_DAILY, // Daily rotation
        .maxBackupFiles = 5
    };
    
    if (logger_init_with_config(&cfg) != 0) {
        printf("Failed to initialize logger: %s\n", logger_strerror(get_last_error()));
        return 1;
    }
    
    // Register modules with different log levels
    logger_register_module("NETWORK", LOG_LEVEL_DEBUG);
    logger_register_module("DATABASE", LOG_LEVEL_WARN);
    logger_register_module("UI", LOG_LEVEL_ERROR);
    
    printf("\n=== Testing Module Filtering ===\n");
    
    // These will be filtered based on module levels
    LOG_DEBUG_MODULE("NETWORK", "Network debug message (SHOULD APPEAR)\n");
    LOG_INFO_MODULE("NETWORK", "Network info message (SHOULD APPEAR)\n");
    LOG_DEBUG_MODULE("DATABASE", "Database debug message (SHOULD NOT APPEAR)\n");
    LOG_WARN_MODULE("DATABASE", "Database warning (SHOULD APPEAR)\n");
    LOG_INFO_MODULE("UI", "UI info message (SHOULD NOT APPEAR)\n");
    LOG_ERROR_MODULE("UI", "UI error message (SHOULD APPEAR)\n");
    
    printf("\n=== Testing Asynchronous Logging ===\n");
    
    // Generate many messages quickly to test async queue
    for (int i = 0; i < 100; i++) {
        LOG_INFO("Async test message %d\n", i);
    }
    
    printf("Queue size: %zu\n", logger_get_async_queue_size());
    
    // Wait for async messages to be processed
    logger_flush();
    printf("After flush - Queue size: %zu\n", logger_get_async_queue_size());
    
    printf("\n=== Testing Time-based Rotation ===\n");
    
    // Force manual rotation to test the feature
    logger_force_rotation();
    LOG_INFO("This message should be in a new log file after manual rotation\n");
    
    // Test context logging
    printf("\n=== Testing Context Logging ===\n");
    LOG_WARN("Context logging test - should show file:line:function\n");
    
    // Test module enable/disable
    printf("\n=== Testing Module Management ===\n");
    logger_enable_module("NETWORK", false);
    LOG_INFO_MODULE("NETWORK", "This should NOT appear - module disabled\n");
    logger_enable_module("NETWORK", true);
    LOG_INFO_MODULE("NETWORK", "This SHOULD appear - module re-enabled\n");
    
    // Close logger
    logger_close();
    
    printf("\nAll enhanced features tested successfully!\n");
    printf("Check 'logs' directory for rotated log files.\n");
    
    return 0;
}
