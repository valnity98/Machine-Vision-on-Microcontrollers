@echo off
setlocal
cd /d "%~dp0\.."

echo Installing build dependencies...
python -m pip install --upgrade pip
python -m pip install -r requirements.txt pyinstaller

echo.
echo Building Windows executable...
pyinstaller --noconfirm --clean --windowed --onefile ^
  --name "STM32_Edge_Vision_Reference_Dashboard" ^
  --icon "app\assets\app_icon.ico" ^
  --add-data "app\ui\reference_window.ui;ui" ^
  --add-data "app\assets\app_icon.ico;assets" ^
  app\src\main.py

echo.
echo EXE created in: dist\STM32_Edge_Vision_Reference_Dashboard.exe
pause
