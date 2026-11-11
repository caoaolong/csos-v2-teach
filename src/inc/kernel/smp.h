#ifndef CSOS_SMP_H
#define CSOS_SMP_H

/* BSP：INIT-SIPI 拉起 MADT 中其余 CPU；AP 仅打印后 hlt */
void init_smp();

void ap_main();

#endif /* CSOS_SMP_H */
