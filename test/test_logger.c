#include "../src/logger.h"
#include <stdio.h>

int main() {
    if (logger_init("app.log", LOG_MAX_SIZE) != 0) {
        printf("Failed to initialize the logger");
        return 1;
    }

    log_message(LOG_LEVEL_INFO, "The application is running\n");
    log_message(LOG_LEVEL_DEBUG, "This is a test message DEBUG\n");
    log_message(LOG_LEVEL_WARN, "This is a warning\n");
    log_message(LOG_LEVEL_ERROR, "This is a mistake.\n");
    log_message(LOG_LEVEL_FATAL, "This is a fatal mistake.\n");

    for (int i=0; i<10; i++) {
        log_message(LOG_LEVEL_DEBUG, "Log message number %d \n", i);
    }

    logger_close();
    printf("Logging completed\n");
    return 0;
}
