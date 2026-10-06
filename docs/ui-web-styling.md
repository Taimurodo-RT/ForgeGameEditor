# Стилизация интерфейса как в вебе

Дата: 2026-10-06. Шаг 13 дорожной карты (редактор интерфейсов). Связано: [editor-ui.md](editor-ui.md),
[stack-choice.md](stack-choice.md).

## Задача

Редактор интерфейсов должен уметь почти всё, что умеет оформление веб-страниц, чтобы в игре можно было
собрать интерфейс уровня Material Design 3 или Bootstrap. Сам редактор говорит языком игр и Figma, а не
веба, но под капотом это CSS: так мы получаем проверенную модель (каскад, переменные, flex, анимации) и
можем проверять себя по браузеру.

## Как это устроено

- **RmlUi лежит в репозитории** (`third_party/rmlui`, версия 6.3), а не скачивается при сборке: мы его
  дописываем. Каждая правка помечена комментарием `// Forge:`.
- **Веб-страницы.** Файл `.html` открывается как документ: корень `<html>`, внутри `<body>`, как в браузере.
  Перевод HTML в RML делает `engine/ui/src/html.cpp`. Первым подключается `ui/web/html.rcss` — стили
  браузера по умолчанию; они помечены `@forge-user-agent;` и проигрывают любому правилу страницы.
- **Проверка по браузеру** — `tools/webcompat`: одни и те же страницы рисуются в Chromium и в движке,
  затем считается доля совпавших пикселей. Страницы лежат в `tools/webcompat/cases`, рядом копии
  Bootstrap 5.3.8 и Beer CSS 5.0.3 (Material Design 3 на чистом CSS).

```
tools/webcompat/run.sh build out [python]   # нужны Node с Playwright и Python с Pillow и numpy
```

Отчёт — `out/report.html`, по каждой странице картинки браузера и движка и счёт.

## Что уже умеет движок сверх RmlUi

| Возможность | Подробности |
|---|---|
| Документ как в браузере | `<html>`/`<body>`, фон `<html>` или `<body>` на всё окно, ширина окна, 1rem = 16px, пробелы между строчными элементами |
| Каскад | `!important`, порядок «стили браузера → стили страницы», `inherit`, `initial`, `unset`, `revert` |
| Селекторы | `:root`, `:is()`, `:where()` (без веса), `:matches()` |
| @-правила | `@media` (ширина, высота, ориентация, тема, `not`, списки), `@supports`, `@layer`, `@charset`, `@import` пропускается |
| Математика | `calc()`, `min()`, `max()`, `clamp()` с любыми единицами, в том числе `calc(100% - 2rem)`; `vmin`, `vmax`, `ch`, `ex`, `turn` внутри них |
| Переменные | `var()` с запасным значением, и в сокращениях (`padding: var(--a) var(--b)`) |
| Цвета | `rgb()`/`hsl()`/`hwb()` в старой и новой записи (`rgb(0 0 0 / .5)`), `color-mix()`, `currentColor`, любые регистры (`RGBA(...)`) |
| Рамки | `border-style`: `none`, `hidden`, `solid` и др. (пока все рисуются сплошными) |
| Логические свойства | `margin-block`, `padding-inline`, `inset-*`, `block-size`, `inline-size`, `border-start-start-radius` и др. (письмо слева направо) |
| Текст | `line-height: normal` по метрикам шрифта, синтетический курсив, `text-align: start/end`, текст прямо внутри flex-контейнера |
| Шрифты | списки семейств (`Inter, "Segoe UI", sans-serif`), подмена неизвестных семейств на свои (`fonts.json`, `aliases`), значки по именам: `<i>home</i>` шрифтом Material Symbols |
| Ключевые слова | `position: sticky` (как `relative`), `overflow: clip/overlay`, `font-weight: lighter/bolder`, `display: list-item/contents/grid` (пока как блок) |

## Что дальше

По убыванию пользы для Material 3 и Bootstrap: `::before`/`::after`, сетка (`display: grid`),
градиенты и картинки в `background`, пунктирные и двойные рамки, `outline`, `text-shadow`,
`mix-blend-mode`, `aspect-ratio`, `object-fit`, маркеры списков, `text-transform: capitalize`,
`text-indent`, `word-spacing`, `line-clamp`.
