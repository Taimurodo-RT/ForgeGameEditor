## Настройки игры: не сюжет, их берёт модуль визуальной новеллы.
define config.name = _("Двор у дома")
define config.version = "0.3"
define build.name = "dvor"
define config.has_voice = False
define audio.yard = "music/yard.ogg"

init python:
    gui.init(1920, 1080)

screen say(who, what):
    text what

transform left_side:
    xalign 0.1

label splashscreen:
    return
