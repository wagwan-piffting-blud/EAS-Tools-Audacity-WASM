@echo off
REM Native build of the x86 VST emulator test driver (x64 host emulating 32-bit guest).
REM Run from PowerShell:  .\build.bat
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo vcvars64 failed & exit /b 1 )
cl /nologo /W3 /O2 /GL /MT mem.c cpu.c loader.c win32_vst.c vst_host.c main_native.c /Fe:vstemu.exe /link /LTCG
if errorlevel 1 ( echo BUILD FAILED ^(vstemu^) & exit /b 1 )
cl /nologo /W3 /O2 /GL /MT mem.c cpu.c loader.c win32_vst.c vst_host.c macrorun.c /Fe:macrorun.exe /link /LTCG
if errorlevel 1 ( echo BUILD FAILED ^(macrorun^) & exit /b 1 )
del *.obj 2>nul
echo built vstemu.exe + macrorun.exe
