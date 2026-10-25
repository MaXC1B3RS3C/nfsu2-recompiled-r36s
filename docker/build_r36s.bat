@echo off
rem Windows shortcut for build.py, the cross-platform build script at the repository root.
rem Drag your game folder (or SPEED2.EXE) onto this file, or run:  build_r36s.bat "C:\path\to\game" [options]
rem Needs Python 3 and Docker Desktop. See BUILD.md.
setlocal
if "%~1"=="" (
  echo Drag your Need for Speed Underground 2 folder or its SPEED2.EXE onto this file.
  pause
  exit /b 1
)
set "PY="
where py >NUL 2>&1 && set "PY=py -3"
if not defined PY where python >NUL 2>&1 && set "PY=python"
if not defined PY (
  echo Python 3 is not installed: https://www.python.org/downloads/
  pause
  exit /b 1
)
%PY% "%~dp0..\build.py" %*
set "RC=%ERRORLEVEL%"
pause
exit /b %RC%
