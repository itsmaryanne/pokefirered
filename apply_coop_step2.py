#!/usr/bin/env python3
# Aplica as edicoes do passo 2 do coop em include/link.h e src/overworld.c.
# Uso: na raiz do pokefirered, rode:   python3 apply_coop_step2.py
# Cada ponto de insercao precisa ser encontrado exatamente uma vez. Se algum
# falhar, o arquivo correspondente fica intocado e o erro e mostrado.
# Pode rodar mais de uma vez: o que ja foi aplicado e ignorado.
import re
import sys

LINK_H = "include/link.h"
OVERWORLD_C = "src/overworld.c"

# (nome, regex do ponto de insercao, texto inserido DEPOIS dele, marcador "ja feito")
H_EDITS = [
    ("define LINKTYPE_COOP",
     r"#define LINKTYPE_CONTEST_EMODE[ \t]+0x6602\n",
     "#define LINKTYPE_COOP                0x7701\n",
     "LINKTYPE_COOP"),
]

O_EDITS = [
    ("#include coop.h",
     r'#include "overworld\.h"\n',
     '#include "coop.h"\n',
     '#include "coop.h"'),
    ("chamada em OverworldBasic",
     r"static void OverworldBasic\(void\)\n\{\n\s+ScriptContext_RunScript\(\);\n\s+RunTasks\(\);\n",
     "    Coop_OverworldTick();\n",
     "Coop_OverworldTick();"),
]


def apply(path, edits):
    with open(path, encoding="utf-8") as f:
        text = f.read()

    ok = True
    for name, pattern, new, marker in edits:
        if marker in text:
            print("[ja feito] %s: %s" % (path, name))
            continue
        matches = list(re.finditer(pattern, text))
        if len(matches) != 1:
            print("[ERRO] %s: %s - ponto de insercao encontrado %d vez(es)"
                  % (path, name, len(matches)))
            ok = False
            continue
        end = matches[0].end()
        text = text[:end] + new + text[end:]
        print("[ok] %s: %s" % (path, name))

    if ok:
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        print("%s NAO foi alterado." % path)
    return ok


if __name__ == "__main__":
    ok_h = apply(LINK_H, H_EDITS)
    ok_o = apply(OVERWORLD_C, O_EDITS)
    sys.exit(0 if (ok_h and ok_o) else 1)
