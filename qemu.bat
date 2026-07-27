qemu-system-x86_64 ^
 -bios edk2\Build\OvmfX64\NOOPT_VS2026\FV\OVMF.fd ^
 -net none ^
 -debugcon file:debug.log ^
 -global isa-debugcon.iobase=0x402