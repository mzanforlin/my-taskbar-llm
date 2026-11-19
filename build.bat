@echo off
echo ========================================================
echo   Compilando TaskbarPerf Monitor (Win32 Nativo C++)
echo ========================================================

REM Encerra o processo anterior caso esteja aberto
taskkill /f /im TaskbarPerf.exe >nul 2>&1

REM Compila usando o compilador w64devkit embutido
"%~dp0w64devkit\bin\g++.exe" -O3 -s -mwindows -municode "%~dp0main.cpp" -o "%~dp0TaskbarPerf.exe" -liphlpapi -lgdi32 -luser32 -ladvapi32

if %ERRORLEVEL% EQU 0 (
    echo.
    echo [SUCESSO] TaskbarPerf.exe gerado com sucesso!
    echo Tamanho do executavel:
    for %%I in ("%~dp0TaskbarPerf.exe") do echo %%~zI bytes
    echo.
    echo Iniciando o monitor...
    start "" "%~dp0TaskbarPerf.exe"
) else (
    echo.
    echo [ERRO] Ocorreu uma falha na compilacao.
)

pause
