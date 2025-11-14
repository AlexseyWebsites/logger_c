@echo off
setlocal

rem Create directories if they don't exist
if not exist "build" mkdir build
if not exist "logs" mkdir logs

rem Compile source files
gcc -c src/logger.c -o build/logger.o -std=c11 -Isrc
gcc test/test_logger.c build/logger.o -o build/test_logger.exe -lws2_32 -lmsvcrt -std=c11

echo Build completed!
echo Executable files in build directory
pause
