# Ferramentas de validação contra o mGBA (opcionais)

Usadas para conferir o port contra a ROM original rodando num emulador de referência. Não são necessárias para jogar.

- `ref.c`  harness que roda a ROM no mGBA (biblioteca estática compilada do código-fonte do mGBA, só o núcleo GBA) e grava:
  áudio (`<prefixo>.pcm` + `<prefixo>.cnt`, amostras por quadro; o mGBA entrega 65536 Hz quando o jogo usa m4a),
  quadros (`<prefixo>_fN.ppm`), um hash por quadro (`REF_HASH=arquivo`) e permite mascarar canais (`REF_MASK=0x30` só PCM,
  `0x0F` só PSG). Teclas: 5º argumento `quadro:mascara,...` (A=1, B=2, Select=4, Start=8, direita=0x10, esquerda=0x20,
  cima=0x40, baixo=0x80).
- `cmp_audio.py`  compara o áudio do port (`PC_AUDIO_DUMP`) com o da referência: alinha por envelope e mede correlação/ganho.

Do lado do port, as variáveis equivalentes são `PC_HASH`, `PC_DUMP`, `PC_AUDIO_DUMP` e `PC_AUDIO_MASK`.
Alinhamento observado: o quadro N do port corresponde ao quadro N+37 do mGBA (diferença de boot).
