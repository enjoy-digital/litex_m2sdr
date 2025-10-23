#ifndef LITESATA_H
#define LITESATA_H

int __init litesata_init(void);
void __exit litesata_exit(void);

void litesata_msi_signal_reader(void);
void litesata_msi_signal_writer(void);

#endif /* LITESATA_H */
