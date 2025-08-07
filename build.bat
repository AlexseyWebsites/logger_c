@echo off
setlocal

rem Compilation of source files
gcc -c src/logger.c -o logger.o -std=c11
gcc test/test_logger.c logger.o -o test_logger.exe -lws2_32 -lmsvcrt -std=c11

echo The assembly is completed
pause
