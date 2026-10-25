@echo off
rem Builds and runs the offline maths checks (x86, same compiler as the plugin).
setlocal
rem Set VCVARS32 to your vcvars32.bat (Visual Studio 2019+ build tools); the default is a local tools folder.
if not defined VCVARS32 set "VCVARS32=%~dp0..\..\tools\vs2019bt\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS32%" >nul || exit /b 1
if not exist "%~dp0..\build\test" mkdir "%~dp0..\build\test"
cl /nologo /O2 /EHsc /std:c++17 /W3 /fp:precise "%~dp0math_test.cpp" /Fo"%~dp0..\build\test\\" /Fe"%~dp0..\build\test\math_test.exe" || exit /b 1
"%~dp0..\build\test\math_test.exe"
