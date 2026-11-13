@echo off
setlocal
cmake -S "%~dp0.." -B "%~dp0..\build\x86\Release" -G "Visual Studio 17 2022" -A Win32 -DCMAKE_INSTALL_PREFIX="%~dp0..\install\x86\Release" %*
if errorlevel 1 exit /b %errorlevel%
cmake --build "%~dp0..\build\x86\Release" --config Release --target install --parallel
exit /b %errorlevel%
