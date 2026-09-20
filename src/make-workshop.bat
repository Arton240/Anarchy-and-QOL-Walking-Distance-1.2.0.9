@echo off
REM ---------------------------------------------------------------------------
REM  make-workshop.bat - put the freshly built plugin into the Workshop item
REM  folder, so uploading is one click with nothing stale in it.
REM
REM  Run this AFTER build-sovietkit.bat. It copies the DLL and the ini, and
REM  nothing else: the README, the preview image and workshopconfig.ini already
REM  live in the item folder and are not regenerated.
REM ---------------------------------------------------------------------------
setlocal

set "SRC=%~dp0"
set "GAME=B:\SteamLibrary\steamapps\common\SovietRepublic"
set "ITEM=%GAME%\media_soviet\workshop_wip\3805103047"
set "DEST=%ITEM%\tesmioloader\build\plugins"

if not exist "%ITEM%" (
  echo.
  echo   The Workshop item folder is not where this script expects it:
  echo     %ITEM%
  echo   Edit GAME at the top of this file if your library is elsewhere.
  echo.
  exit /b 1
)

if not exist "%SRC%sovietkit.dll" (
  echo.
  echo   sovietkit.dll is not here. Run build-sovietkit.bat first.
  echo.
  exit /b 1
)

if not exist "%DEST%" mkdir "%DEST%"

copy /Y "%SRC%sovietkit.dll" "%DEST%\sovietkit.dll" >nul || goto :fail
copy /Y "%SRC%sovietkit.ini" "%DEST%\sovietkit.ini" >nul || goto :fail

REM The same two files also go into the live game folder, so what you upload is
REM what you have been playing.
set "LIVE=%GAME%\tesmioloader\build\plugins"
if exist "%LIVE%" (
  copy /Y "%SRC%sovietkit.dll" "%LIVE%\sovietkit.dll" >nul
  copy /Y "%SRC%sovietkit.ini" "%LIVE%\sovietkit.ini" >nul
)

echo.
echo   Workshop item updated:
echo     %DEST%\sovietkit.dll
echo     %DEST%\sovietkit.ini
echo.
echo   Close the game before uploading. The item is 3805103047.
echo.
exit /b 0

:fail
echo.
echo   Copy failed - is the game running, or the file open somewhere?
echo.
exit /b 1
