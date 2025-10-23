#ifndef LITESATA_H
#define LITESATA_H

int __init litesata_init(void);
void __exit litesata_exit(void);

void litesata_msi_signal_reader(void); /* SATA_SECTOR2MEM done */
void litesata_msi_signal_writer(void); /* SATA_MEM2SECTOR done */

#endif /* LITESATA_H */
