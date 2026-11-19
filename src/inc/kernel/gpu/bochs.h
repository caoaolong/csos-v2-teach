#ifndef CSOS_BOCHS_H
#define CSOS_BOCHS_H

struct boot_info;

/* Bochs VBE dispi modesetting（QEMU -vga std）。
 * 在 init_vmm 映射显存之前调用：找到 1234:1111 则设高分辨率并改写
 * boot_info，后续映射/控制台/compositor 全走新尺寸；找不到则保持 GOP。 */
void bochs_modeset(struct boot_info *boot);

#endif /* CSOS_BOCHS_H */
