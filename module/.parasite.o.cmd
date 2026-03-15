savedcmd_/data/share/cyber/CoKernel/module/parasite.o := x86_64-linux-gnu-ld -m elf_x86_64 -z noexecstack --no-warn-rwx-segments   -r -o /data/share/cyber/CoKernel/module/parasite.o @/data/share/cyber/CoKernel/module/parasite.mod  ; ./tools/objtool/objtool --hacks=jump_label --hacks=noinstr --hacks=skylake --ibt --orc --retpoline --rethunk --sls --static-call --uaccess --prefix=16  --link  --module /data/share/cyber/CoKernel/module/parasite.o

/data/share/cyber/CoKernel/module/parasite.o: $(wildcard ./tools/objtool/objtool)
