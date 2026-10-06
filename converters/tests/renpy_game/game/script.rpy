label start:
    $ b_name = "Борис"
    scene bg yard
    with fade
    play music "audio/morning.ogg"

    "Утро во дворе. Пахнет {i}сиренью{/i}."

    show alice happy at left
    a "Привет, [player]! Пойдёшь с нами?"

    menu:
        "Пойду.":
            $ coins += 2
            jump walk
        "Только если Борис идёт." if met_boris:
            a smile "Он идёт, не волнуйся."
            jump walk
        "Нет, дела.":
            a sad "Жаль..."
            return

label walk:
    call chapter_one
    if coins >= 7:
        a "Ты сегодня богат!"
    elif coins > 0:
        a "Хватит на мороженое."
    else:
        a "Пусто в карманах."
    $ renpy.pause(0.5)
    $ result = greet(player)
    "Конец."
    return
