mkdir bootloader\CsosBootPkg
mkdir bootloader\MdeModulePkg\Application\HelloWorld

cp -r -f edk2\build.bat bootloader\build.bat

cp -r -f edk2\CsosBootPkg bootloader\CsosBootPkg
cp -r -f edk2\MdeModulePkg\Application\HelloWorld bootloader\MdeModulePkg\Application\HelloWorld