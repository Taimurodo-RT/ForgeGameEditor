# Картинки каталога шаблонов

Каталог `games/templates.json` называет шаблоны окна «Новая игра из шаблона» редактора: название, описание,
модуль, папку игры шаблона и картинку карточки. Картинки лежат здесь.

- `old-mine.png` (480×270) — кадр самой «Старой шахты» сразу после начала новой игры. Снят игрой из сборки и
  уменьшен вдвое:

  ```
  forge_slice --play --screenshot old-mine-full.png --frames 90 --size 960x540 --user <временная папка>
  python3 -c "from PIL import Image; Image.open('old-mine-full.png').convert('RGB').reduce(2).save('old-mine.png', optimize=True)"
  ```
