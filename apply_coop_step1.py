#!/usr/bin/env python3
# Aplica as edicoes do passo 1 do coop em include/link.h e src/link.c.
# Uso: na raiz do pokefirered, rode:   python3 apply_coop_step1.py
# Cada ponto de insercao precisa ser encontrado exatamente uma vez. Se algum
# falhar, o arquivo correspondente fica intocado e o erro e mostrado.
# Pode rodar mais de uma vez: o que ja foi aplicado e ignorado.
import re
import sys

LINK_H = "include/link.h"
LINK_C = "src/link.c"

# (nome, regex do ponto de insercao, texto inserido DEPOIS dele, marcador "ja feito")
H_EDITS = [
    ("define LINKCMD_COOP_POS",
     r"#define LINKCMD_SEND_HELD_KEYS[ \t]+0xCAFE\n",
     "#define LINKCMD_COOP_POS                0x6A01\n",
     "LINKCMD_COOP_POS"),
    ("prototipo StartSendingCoopPos",
     r"bool8 SendBlock\(u8, const void \*, u16\);\n",
     "void StartSendingCoopPos(void);\n",
     "StartSendingCoopPos"),
]

C_EDITS = [
    ("#include coop.h",
     r'#include "link\.h"\n',
     '#include "coop.h"\n',
     '#include "coop.h"'),
    ("declaracao antecipada",
     r"static void LinkCB_SendHeldKeys\(void\);\n",
     "static void LinkCB_SendCoopPos(void);\n",
     "static void LinkCB_SendCoopPos(void);"),
    ("case em ProcessRecvCmds",
     r"case LINKCMD_SEND_HELD_KEYS:\n\s+gLinkPartnersHeldKeys\[i\] = gRecvCmds\[i\]\[1\];\n\s+break;\n",
     "        case LINKCMD_COOP_POS:\n"
     "            Coop_OnPosCmd(i, &gRecvCmds[i][1]);\n"
     "            break;\n",
     "Coop_OnPosCmd"),
    ("case em BuildSendCmd",
     r"gSendCmd\[1\] = gHeldKeyCodeToSend;\n\s+break;\n",
     "    case LINKCMD_COOP_POS:\n"
     "        gSendCmd[0] = LINKCMD_COOP_POS;\n"
     "        Coop_BuildPosCmd(&gSendCmd[1]);\n"
     "        break;\n",
     "Coop_BuildPosCmd"),
    ("funcoes StartSendingCoopPos e LinkCB_SendCoopPos",
     r"static void LinkCB_SendHeldKeys\(void\)\n\{\n[^}]*\}\n",
     "\n"
     "void StartSendingCoopPos(void)\n"
     "{\n"
     "    gLinkCallback = LinkCB_SendCoopPos;\n"
     "}\n"
     "\n"
     "static void LinkCB_SendCoopPos(void)\n"
     "{\n"
     "    if (gReceivedRemoteLinkPlayers == TRUE)\n"
     "        BuildSendCmd(LINKCMD_COOP_POS);\n"
     "}\n",
     "void StartSendingCoopPos(void)\n{"),
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
    ok_c = apply(LINK_C, C_EDITS)
    sys.exit(0 if (ok_h and ok_c) else 1)
