label chapter_one:
    show boris at right with dissolve
    b "Я Борис."
    $ met_boris = True
    $ i = 0
    while i < 2:
        b "Раз!"
        $ i += 1
    menu:
        b "Дальше?"
        "Да.":
            pass
        "Нет.":
            jump .stay
    return

label .stay:
    b "Тогда сидим."
    return
