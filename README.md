# Forge (ForgeGameEditor)

Ядро конструктора 2D-игр. Цель: большие 2D-игры на ПК, стабильные 60 FPS на среднем ПК.
Архитектура: `docs/core-architecture.md`, выбор стека: `docs/stack-choice.md`. Дорожная карта: https://claude.ai/artifact/GCrpZ4AH37XUrDU6kioobh

## Что уже есть (шаги 1–3)

- `engine/core` — память по категориям, `LinearArena` (покадровая память), `BlockPool`,
  `HandlePool` (ссылки с поколениями), система задач с work stealing, логи, Tracy.
- `engine/data` — рефлексия типов (одно описание на всё: инспектор, сохранение, скрипты), GUID,
  текстовый формат JSON для проекта с миграциями версий, бинарный формат для игры.
- `engine/assets` — база ассетов (SQLite, поиск по-русски), сборка ассетов: `.meta` с GUID рядом с файлом,
  пересборка только изменённого на всех ядрах, импорт картинок.
- `engine/world` — тайловый мир любого размера из чанков 64×64: подгрузка вокруг камеры в фоновых задачах,
  выгрузка за спиной, изменённые чанки сохраняются сжатыми в файлы-регионы. Генераторы-образцы: вид сбоку (как Terraria)
  и вид сверху (как Factorio).
- `engine/render` — отрисовка тайлов: один вызов отрисовки на слой, на GPU загружаются только новые и изменённые чанки.
  Шейдеры пишутся на GLSL и при сборке компилируются для Vulkan (SPIR-V) и Direct3D 12 (DXBC) инструментом `tools/shaderc`.
- `engine/platform` — окно и GPU через SDL3 (Vulkan / D3D12 / Metal), цикл кадров, статистика кадра.
- `apps/sandbox` — окно, 200 000 частиц и 10 000 задач в каждом кадре; FPS в заголовке.
- `apps/world_demo` — мир 64k × 64k тайлов: полёт, зум, копать и строить мышью.
- `tests` — модульные тесты (doctest), `bench` — замер системы задач.

## Сборка

Нужны CMake 3.24+, Ninja или Visual Studio 2022, компилятор с C++20 и git.
Зависимости (SDL3, Tracy, doctest, glslang, SPIRV-Cross и др.) скачиваются при первой настройке.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Windows (Visual Studio 2022): `cmake -S . -B build -G "Visual Studio 17 2022"`, затем открыть `build/Forge.sln`
или `cmake --build build --config Release`.

## Запуск

```
build/apps/sandbox/forge_sandbox              # окно, FPS в заголовке
build/apps/sandbox/forge_sandbox --no-vsync   # без ограничения кадров
build/apps/sandbox/forge_sandbox --headless 600
build/apps/world_demo/forge_world_demo        # мир, вид сбоку
build/apps/world_demo/forge_world_demo --topdown --no-vsync
build/apps/world_demo/forge_world_demo --screenshot world.png   # отрисовать без окна в файл
build/tests/forge_tests
build/bench/forge_bench_jobs
build/bench/forge_bench_data
build/bench/forge_bench_assets [число файлов]
build/bench/forge_bench_world [--topdown] [--speed тайлов_в_секунду]
```

Управление в `forge_world_demo`: WASD или стрелки — движение (Shift быстрее), колесо — зум к курсору,
левая кнопка — копать, правая — строить, F — автополёт через мир, 1 / 2 — вид сбоку / вид сверху,
F5 — сохранить. Изменения мира сохраняются и при выходе в папку `saves` рядом с местом запуска:
на диск пишутся только изменённые чанки, сгруппированные в файлы-регионы 32×32 чанка.

Профилирование: запустите Tracy Profiler 0.14.1 и подключитесь к запущенной песочнице.
Сборка без профилировщика: `-DFORGE_PROFILE=OFF`.
