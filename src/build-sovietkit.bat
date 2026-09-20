@echo off
REM ===========================================================================
REM  build-sovietkit.bat - builds sovietkit.dll with MSVC and installs it.
REM
REM  Run this from the folder it sits in. It needs, beside it:
REM      sovietkit.cpp   sovietkit.ini   an_sites.h   skn_*.h   tc_sites.h   mod_*.inc   tsm_diag.h
REM      tesmio_api.h   tesmio_plugin.h
REM
REM  ONE TRANSLATION UNIT - the headers are included from sovietkit.cpp and
REM  never appear on the command line.
REM
REM  Why MSVC and not a cross-build: __try / __except. Without structured
REM  exception handling a bad read takes the game down instead of switching the
REM  plugin off and saying why.
REM ===========================================================================

setlocal
set "HERE=%~dp0"
set "GAME=B:\SteamLibrary\steamapps\common\SovietRepublic"
set "PLUGINS=%GAME%\tesmioloader\build\plugins"
set "LOADERINI=%GAME%\tesmioloader\build\tesmioloader.ini"

echo.
echo   sovietkit - build
echo   -----------------
echo.

if not exist "%HERE%sovietkit.cpp" (
  echo   sovietkit.cpp is not next to this script. Nothing to build.
  pause & exit /b 1
)

REM ---- find Visual Studio ---------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo   vswhere.exe not found - is Visual Studio Build Tools installed?
  pause & exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo   No installation with the x64 C++ tools. Install "Desktop development
  echo   with C++" in the Visual Studio Installer and run this again.
  pause & exit /b 1
)

echo   using %VSPATH%
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo   vcvars64 failed & pause & exit /b 1 )

REM ---- build ----------------------------------------------------------------
if not exist "%HERE%obj" mkdir "%HERE%obj"

echo   compiling...
cl /nologo /LD /MT /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS ^
   "%HERE%sovietkit.cpp" ^
   /Fe"%HERE%sovietkit.dll" /Fo"%HERE%obj\\" /Fd"%HERE%obj\\" ^
   /link /INCREMENTAL:NO /OPT:REF user32.lib

if errorlevel 1 ( echo. & echo   BUILD FAILED - nothing was installed. & pause & exit /b 1 )
if not exist "%HERE%sovietkit.dll" ( echo   no dll produced & pause & exit /b 1 )

REM ---- install --------------------------------------------------------------
if not exist "%PLUGINS%" (
  echo.
  echo   Built, but %PLUGINS%
  echo   does not exist - copy sovietkit.dll there by hand.
  pause & exit /b 0
)

copy /Y "%HERE%sovietkit.dll" "%PLUGINS%\sovietkit.dll" >nul

REM  The ini is NOT overwritten once it exists - your rule switches and any
REM  [sites] corrections live there and a rebuild must not throw them away.
if exist "%PLUGINS%\sovietkit.ini" (
  echo   sovietkit.ini already installed - left alone.
) else (
  copy /Y "%HERE%sovietkit.ini" "%PLUGINS%\sovietkit.ini" >nul
  echo   sovietkit.ini installed.
)

REM ---- enable it in the loader ----------------------------------------------
if exist "%LOADERINI%" (
  findstr /b /i /c:"sovietkit=" "%LOADERINI%" >nul 2>&1
  if errorlevel 1 (
    echo sovietkit=1>>"%LOADERINI%"
    echo   added sovietkit=1 to tesmioloader.ini
  )
) else (
  echo   NOTE: %LOADERINI%
  echo         not found - add  sovietkit=1  under [plugins] by hand.
)

REM ---- what to look for ------------------------------------------------------
echo.
echo   built and installed.
echo.
echo   MAKE A BACKUP SAVE BEFORE THE FIRST TEST. Anarchy builds things the
echo   game would have refused, and refusals exist for reasons.
echo.
echo   Run the game through tesmiolauncher.exe, then open
echo       %GAME%\tesmioloader\build\sovietkit.log
echo.
echo   It should end with:
echo       ready: anarchy on, skins on, trackcrew on
echo.
echo   and above that, one block per part. The lines worth reading:
echo       114 of 114 match this build; 0 do not   (anarchy's byte edits)
echo       all 5 sites match ...                   (skins' offsets)
echo       hooked exe+0x6ac6e0                     (trackcrew's one hook)
echo.
echo   Then in game:
echo       Alt+F        anarchy on and off. The word ANARCHY appears top right.
echo       Ctrl+Alt+R   the full report - all three parts, then the counters
echo       Ctrl+Alt+K   zero every counter
echo.
echo   Any part can be switched off at the top of sovietkit.ini without
echo   touching the other two.
echo.
echo   A line reading "the game updated" means the exe is not v1.1.1.9 and
echo   nothing was hooked. That is the design, not a failure.
echo.
pause
