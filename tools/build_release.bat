@echo off
rem Builds the release DLL (no measuring log, debug boxes or compare key: OC_RELEASE) into build\release\.
setlocal
rem Set MSBUILD to your MSBuild.exe (Visual Studio 2019 build tools, v142); the default is a local tools folder.
if not defined MSBUILD set "MSBUILD=%~dp0..\..\tools\vs2019bt\MSBuild\Current\Bin\MSBuild.exe"
set "CL=/DOC_RELEASE"
"%MSBUILD%" "%~dp0..\OcclusionCulling.vcxproj" -property:Configuration=Release -property:Platform=Win32 "-property:OutDir=%~dp0..\build\release\\" "-property:IntDir=%~dp0..\build\release-obj\\" -nologo -verbosity:minimal || exit /b 1
