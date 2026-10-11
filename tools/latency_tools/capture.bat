@echo off
rem MGPU latency instrument - one capture. NOT part of the add-on, never shipped.
rem PresentMon (Intel, open source, signed) reads Windows' own present, display
rem and input events from OUTSIDE the game: nothing is loaded into the game.
rem
rem Double-click it. It asks for administrator rights (Windows' event tracing
rem needs them), then for the game and a run name, then waits for F11 in the
rem game: 60 seconds are recorded, and this window says whether the file was
rem written. The capture is ..\latency_runs\RUN-NAME.csv
setlocal EnableExtensions

net session >nul 2>&1
if errorlevel 1 (
  echo Asking for administrator rights...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)

set "HERE=%~dp0"
set "OUT=%HERE%..\latency_runs"
if not exist "%OUT%" mkdir "%OUT%"
set "PM=%HERE%PresentMon-2.6.0-x64.exe"
if not exist "%PM%" (
  echo PresentMon-2.6.0-x64.exe is not next to this file: %HERE%
  pause
  exit /b 1
)

echo.
echo  MGPU latency capture - PresentMon, outside the game
echo.
echo   1  Dawnwalker             Dawnwalker.exe
echo   2  Skyrim Special Edition SkyrimSE.exe
echo   3  Resident Evil 4        re4.exe
echo   4  Rise of the Tomb Raider ROTTR.exe
echo   5  Cyberpunk 2077         Cyberpunk2077.exe
echo   6  Starfield              Starfield.exe
echo   7  Red Dead Redemption 2  RDR2.exe
echo.

:ask_game
set "G="
set "EXE="
set "PCL="
set /p "G=Game number: "
if "%G%"=="1" set "EXE=Dawnwalker.exe"
if "%G%"=="2" set "EXE=SkyrimSE.exe"
if "%G%"=="3" set "EXE=re4.exe"
if "%G%"=="4" set "EXE=ROTTR.exe"
if "%G%"=="5" set "EXE=Cyberpunk2077.exe"
if "%G%"=="6" set "EXE=Starfield.exe"
if "%G%"=="7" set "EXE=RDR2.exe"
if not defined EXE (
  echo Not a number from the list.
  goto ask_game
)
rem PC latency from the game's own markers, on every title: a title that sends
rem none just leaves the column empty (18:35 - it was Dawnwalker only, and the
rem Cyberpunk reference ran without it).
set "PCL=--track_pc_latency"

:ask_run
set "RUN="
set /p "RUN=Run name (the one Claude gave, e.g. DW-L1): "
if not defined RUN goto ask_run
if exist "%OUT%\%RUN%.csv" goto run_taken
if exist "%OUT%\%RUN%-*.csv" goto run_taken
goto run_ok
:run_taken
echo %RUN% already has a capture - use another run name.
goto ask_run
:run_ok

:ask_multi
set "MULTI="
set /p "MULTI=More than one recording in this launch, e.g. Reflex on / off / on? (y/n): "
if /i "%MULTI%"=="y" goto multi
if /i "%MULTI%"=="n" goto single
goto ask_multi

:single
echo.
echo  Recording %RUN% of %EXE%.
echo  1. Start the game if it is not running and go to the test spot.
echo  2. Press F11 in the game. PresentMon records 60 seconds and stops by itself.
echo  3. Move and turn during the 60 seconds, the same way in both runs of a pair.
echo  Leave this window open. It says DONE when the file is written.
echo.
"%PM%" --process_name %EXE% --output_file "%OUT%\%RUN%.csv" --hotkey F11 --timed 60 --terminate_after_timed %PCL%
goto report

:multi
echo.
echo  Recording %RUN% of %EXE%, several recordings in one launch.
echo  1. Start the game if it is not running and go to the test spot.
echo  2. Press F11: 60 seconds are recorded as %RUN%-1. Move and turn the same way each time.
echo  3. Change the setting in the game, go back to the spot, press F11 again: %RUN%-2. And so on.
echo     Wait for each 60 seconds to end before the next F11 - F11 during a recording stops it early.
echo  4. Quit the game when the last recording is done: PresentMon stops by itself.
echo  Leave this window open. It lists every file written.
echo.
"%PM%" --process_name %EXE% --output_file "%OUT%\%RUN%.csv" --hotkey F11 --timed 60 --terminate_on_proc_exit %PCL%

:report
echo.
rem PresentMon adds the recording number to the name when F11 starts it: RUN-1.csv
set "FOUND="
for %%F in ("%OUT%\%RUN%.csv" "%OUT%\%RUN%-*.csv") do if exist "%%~F" (
  set "FOUND=1"
  echo  DONE: %%~fF  ^(%%~zF bytes^)
)
if not defined FOUND echo  NO FILE WAS WRITTEN for %RUN%. Tell Claude what this window shows above.
echo.
pause
