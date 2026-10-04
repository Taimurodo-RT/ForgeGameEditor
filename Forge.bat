@echo off
rem Запуск конструктора Forge двойным щелчком.
rem Собирает редактор, если его ещё нет или код изменился, и открывает окно.
rem Forge.bat --update  сначала скачивает свежую версию с GitHub.
chcp 65001 >nul
setlocal
cd /d "%~dp0"
set "EXE=build\apps\editor\Release\forge_editor.exe"

where cmake >nul 2>nul
if errorlevel 1 (
    echo Не найден CMake. Установите его командой: winget install Kitware.CMake
    goto fail
)

if /i "%~1"=="--update" (
    echo Скачиваю свежую версию...
    git pull --ff-only
    if errorlevel 1 (
        echo Не получилось обновить: в папке есть свои изменения или нет связи с GitHub.
        goto fail
    )
)

if not exist "build\CMakeCache.txt" (
    echo Первая настройка: скачиваются библиотеки, это займёт несколько минут...
    cmake -S . -B build
    if errorlevel 1 goto fail
)

echo Собираю редактор...
cmake --build build --config Release --target forge_editor_app
if errorlevel 1 goto fail

start "" "%EXE%"
exit /b 0

:fail
echo.
echo Конструктор не запустился. Пришлите текст выше в чат проекта.
pause
exit /b 1
