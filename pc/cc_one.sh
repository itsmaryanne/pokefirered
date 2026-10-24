#!/bin/bash
# Compila um arquivo C do pokefirered para x86 32 bits: cpp -> preproc (textos GBA) -> gcc -m32
# Arquivos com asm ARM inline passam antes por pc/patch_src.pl (em cópia; o original não muda).
f="$1"; b=$(basename "$f" .c)
case " script m4a main main_menu union_room dma3_manager battle_controller_opponent librfu_intr multiboot bg gpu_regs naming_screen trainer_card librfu_stwi librfu_rfu overworld " in
  *" $b "*) mkdir -p build_pc/patched; perl pc/patch_src.pl "$b" < "$f" > build_pc/patched/$b.c; src=build_pc/patched/$b.c ;;
  *) src="$f" ;;
esac
FLAGS="-iquote include -iquote src -DFIRERED -DREVISION=0 -DENGLISH -DMODERN=1 -DPC_PORT"
gcc -E -std=gnu17 -m32 -Wno-trigraphs -include build_pc/gba_macro_pc.h $FLAGS "$src" 2>/dev/null \
 | tools/preproc/preproc -i "$f" charmap.txt \
 | gcc -m32 -x c -std=gnu17 -O1 -w -fno-strict-aliasing -fwrapv -fno-common $FLAGS \
     -c -o build_pc/$b.o - 2> build_pc/log/$b.log
if [ $? -eq 0 ]; then echo "OK $b"; else echo "FAIL $b"; fi
