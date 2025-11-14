@echo off
setlocal

rem Creating directories if there are none
if not exist "build" mkdir build
if not exist "logs" mkdir logs

rem Compilation of source files
gcc -c src/logger.c -o build/logger.o -std=c11 -Isrc
gcc test/test_logger.c build/logger.o -o build/test_logger.exe -lws2_32 -lmsvcrt -std=c11

echo The build is complete!
echo Executable files in the build
pause folder
