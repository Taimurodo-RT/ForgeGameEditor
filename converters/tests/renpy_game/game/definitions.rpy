# Персонажи и то, что есть с самого начала.
define a = Character("Алиса", who_color="#c8a2e0")
define b = DynamicCharacter("b_name", image="boris")
define narrator = Character(None)

default coins = 5
default met_boris = False
default persistent.endings = 0

image bg yard = "images/yard.png"

init python:
    def greet(name):
        return "Привет, " + name

image boris happy = "images/boris_happy.png"
image boris sad:
    "images/boris_sad.png"
