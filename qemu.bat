cp -f edk2\Build\CsosBootPkg\DEBUG_VS2026\X64\GopMode.efi qemu/hda-contents/GopMode.efi

qemu-system-x86_64 ^
 -bios edk2\Build\OvmfX64\NOOPT_VS2026\FV\OVMF.fd ^
 -drive format=raw,file=fat:rw:qemu/hda-contents ^
 -net none ^
 -serial stdio ^
 -debugcon file:debug.log ^
 -global isa-debugcon.iobase=0x402