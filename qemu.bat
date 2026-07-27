cp -f edk2\Build\MdeModule\DEBUG_VS2026\X64\HelloWorld.efi qemu/hda-contents/

qemu-system-x86_64 ^
 -bios edk2\Build\OvmfX64\NOOPT_VS2026\FV\OVMF.fd ^
 -drive format=raw,file=fat:rw:qemu/hda-contents ^
 -net none ^
 -debugcon file:debug.log ^
 -global isa-debugcon.iobase=0x402