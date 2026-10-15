@echo off
rem Spall build script (MSVC, no CMake).
rem Usage: build.bat [release^|debug^|dist] [all^|app^|test^|clean]
rem Third-party libraries are compiled once per configuration and cached in build\<cfg>.
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "CFG=release"
set "TARGET=all"
:parse
if "%~1"=="" goto parsed
if /i "%~1"=="debug"   set "CFG=debug"
if /i "%~1"=="release" set "CFG=release"
if /i "%~1"=="dist"    set "CFG=dist"
if /i "%~1"=="all"    set "TARGET=all"
if /i "%~1"=="app"     set "TARGET=app"
if /i "%~1"=="test"    set "TARGET=test"
if /i "%~1"=="clean"   set "TARGET=clean"
shift
goto parse
:parsed

if /i "%TARGET%"=="clean" goto clean

call :find_msvc
if errorlevel 1 exit /b 1

set "OUT=build\%CFG%"
set "OBJ=%OUT%\obj"
for %%D in (glfw imgui app test) do if not exist "%OBJ%\%%D" mkdir "%OBJ%\%%D"

if /i "%CFG%"=="debug" goto cfg_debug
rem dist is release with the C runtime linked in, so the exe runs without the VC++ redistributable.
set "CRT=/MD"
if /i "%CFG%"=="dist" set "CRT=/MT"
set "CFLAGS=/nologo /O2 /Zi /FS %CRT% /DNDEBUG"
set "LFLAGS=/nologo /DEBUG /OPT:REF /OPT:ICF"
goto cfg_done
:cfg_debug
set "CFLAGS=/nologo /Od /Zi /FS /MDd /D_DEBUG"
set "LFLAGS=/nologo /DEBUG"
:cfg_done

set "DEFS=/DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS"
set "INC=/Isrc /Ithird_party\imgui /Ithird_party\imgui\backends /Ithird_party\glfw\include"
set "CXXFLAGS=%CFLAGS% %DEFS% %INC% /std:c++20 /EHsc /W4 /permissive- /utf-8 /MP"
set "SYSLIBS=opengl32.lib gdi32.lib user32.lib shell32.lib dwmapi.lib ole32.lib"

rem ---------------------------------------------------------------- GLFW
if exist "%OUT%\glfw.lib" goto glfw_done
echo [glfw] compiling (one-time)
set "G=third_party\glfw\src"
cl %CFLAGS% /W3 /MP /D_GLFW_WIN32 /D_CRT_SECURE_NO_WARNINGS /c /Fo%OBJ%\glfw\ /Fd%OUT%\glfw.pdb ^
  %G%\context.c %G%\init.c %G%\input.c %G%\monitor.c %G%\platform.c %G%\vulkan.c %G%\window.c ^
  %G%\egl_context.c %G%\osmesa_context.c %G%\null_init.c %G%\null_monitor.c %G%\null_window.c ^
  %G%\null_joystick.c %G%\win32_module.c %G%\win32_time.c %G%\win32_thread.c %G%\win32_init.c ^
  %G%\win32_joystick.c %G%\win32_monitor.c %G%\win32_window.c %G%\wgl_context.c
if errorlevel 1 goto fail
lib /nologo /OUT:%OUT%\glfw.lib %OBJ%\glfw\*.obj
if errorlevel 1 goto fail
:glfw_done

rem ---------------------------------------------------------------- Dear ImGui
if exist "%OUT%\imgui.lib" goto imgui_done
echo [imgui] compiling (one-time)
set "I=third_party\imgui"
cl %CFLAGS% %DEFS% %INC% /std:c++20 /EHsc /W3 /MP /c /Fo%OBJ%\imgui\ /Fd%OUT%\imgui.pdb ^
  %I%\imgui.cpp %I%\imgui_draw.cpp %I%\imgui_tables.cpp %I%\imgui_widgets.cpp %I%\imgui_demo.cpp ^
  %I%\backends\imgui_impl_glfw.cpp %I%\backends\imgui_impl_opengl3.cpp
if errorlevel 1 goto fail
lib /nologo /OUT:%OUT%\imgui.lib %OBJ%\imgui\*.obj
if errorlevel 1 goto fail
:imgui_done

if /i "%TARGET%"=="test" goto build_tests

rem ---------------------------------------------------------------- Spall.exe
echo [app] compiling
if exist "%OBJ%\app\*.obj" del /q "%OBJ%\app\*.obj"
(for /r src %%F in (*.cpp) do @echo "%%F") > "%OUT%\app_sources.rsp"
cl %CXXFLAGS% /c /Fo%OBJ%\app\ /Fd%OUT%\app.pdb @%OUT%\app_sources.rsp
if errorlevel 1 goto fail
rc /nologo /fo %OUT%\spall.res res\spall.rc
if errorlevel 1 goto fail
link %LFLAGS% /OUT:%OUT%\Spall.exe /PDB:%OUT%\Spall.pdb /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup ^
  %OBJ%\app\*.obj %OUT%\spall.res %OUT%\imgui.lib %OUT%\glfw.lib %SYSLIBS%
if errorlevel 1 goto fail
echo Built %OUT%\Spall.exe
if /i "%TARGET%"=="app" goto done

rem ---------------------------------------------------------------- SpallTests.exe
:build_tests
echo [tests] compiling
if exist "%OBJ%\test\*.obj" del /q "%OBJ%\test\*.obj"
(for %%F in (tests\*.cpp) do @echo "%%F") > "%OUT%\test_sources.rsp"
for %%F in (src\core\json.cpp src\core\log.cpp src\core\sysmem.cpp src\core\image_io.cpp src\data\explosive_db.cpp src\data\projectile_db.cpp src\physics\penetration.cpp src\physics\mpm_material.cpp src\physics\projectile_shape.cpp src\physics\fuze.cpp src\sim\mpm_scene.cpp src\sim\armour_run.cpp src\sim\armour_case_io.cpp src\sim\witness_readout.cpp src\render\blackbody.cpp src\app\settings_ini.cpp) do @if exist "%%F" echo "%%F">> "%OUT%\test_sources.rsp"
cl %CXXFLAGS% /c /Fo%OBJ%\test\ /Fd%OUT%\tests.pdb @%OUT%\test_sources.rsp
if errorlevel 1 goto fail
link %LFLAGS% /OUT:%OUT%\SpallTests.exe /PDB:%OUT%\SpallTests.pdb /SUBSYSTEM:CONSOLE %OBJ%\test\*.obj ole32.lib windowscodecs.lib
if errorlevel 1 goto fail
echo Built %OUT%\SpallTests.exe
goto done

rem ---------------------------------------------------------------- helpers
:find_msvc
where cl >nul 2>nul
if not errorlevel 1 exit /b 0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_msvc
set "VSINSTALL="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL goto no_msvc
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul
if errorlevel 1 goto no_msvc
exit /b 0
:no_msvc
echo ERROR: MSVC x64 compiler not found. Install "Visual Studio Build Tools" with the C++ workload.
exit /b 1

:clean
if exist build rmdir /s /q build
echo Cleaned.
exit /b 0

:fail
echo BUILD FAILED
exit /b 1

:done
exit /b 0
