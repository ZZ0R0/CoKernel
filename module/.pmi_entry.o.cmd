savedcmd_/data/share/cyber/CoKernel/module/pmi_entry.o :=  x86_64-linux-gnu-gcc-14 -Wp,-MMD,/data/share/cyber/CoKernel/module/.pmi_entry.o.d -nostdinc -I/usr/src/linux-headers-6.12.73+deb13-common/arch/x86/include -I./arch/x86/include/generated -I/usr/src/linux-headers-6.12.73+deb13-common/include -I./include -I/usr/src/linux-headers-6.12.73+deb13-common/arch/x86/include/uapi -I./arch/x86/include/generated/uapi -I/usr/src/linux-headers-6.12.73+deb13-common/include/uapi -I./include/generated/uapi -include /usr/src/linux-headers-6.12.73+deb13-common/include/linux/compiler-version.h -include /usr/src/linux-headers-6.12.73+deb13-common/include/linux/kconfig.h -D__KERNEL__ -fmacro-prefix-map=/usr/src/linux-headers-6.12.73+deb13-common/= -D__ASSEMBLY__ -fno-PIE -m64 -DCC_USING_FENTRY -g  -DMODULE  -DKBUILD_MODNAME='"parasite"' -D__KBUILD_MODNAME=kmod_parasite -c -o /data/share/cyber/CoKernel/module/pmi_entry.o /data/share/cyber/CoKernel/module/pmi_entry.S 

source_/data/share/cyber/CoKernel/module/pmi_entry.o := /data/share/cyber/CoKernel/module/pmi_entry.S

deps_/data/share/cyber/CoKernel/module/pmi_entry.o := \
  /usr/src/linux-headers-6.12.73+deb13-common/include/linux/compiler-version.h \
    $(wildcard include/config/CC_VERSION_TEXT) \
  /usr/src/linux-headers-6.12.73+deb13-common/include/linux/kconfig.h \
    $(wildcard include/config/CPU_BIG_ENDIAN) \
    $(wildcard include/config/BOOGER) \
    $(wildcard include/config/FOO) \

/data/share/cyber/CoKernel/module/pmi_entry.o: $(deps_/data/share/cyber/CoKernel/module/pmi_entry.o)

$(deps_/data/share/cyber/CoKernel/module/pmi_entry.o):
