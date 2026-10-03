# Script mode helper for forge_embed_text: IN, OUT, SYMBOL.
file(READ ${IN} hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(LENGTH "${hex}" hex_len)
math(EXPR size "${hex_len} / 2")
file(WRITE ${OUT} "// Generated from ${IN}. Do not edit.\nextern const unsigned char ${SYMBOL}[] = {${bytes}0};\nextern const unsigned long long ${SYMBOL}_size = ${size};\n")
