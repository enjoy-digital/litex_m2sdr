CFLAGS  = -O2 -Wall -g -I../kernel -Iliblitepcie -Iliblitexm2sdr -MMD -fPIC
LDFLAGS = -g
CC      = $(CROSS_COMPILE)gcc
AR      = ar

PROGS=litex_m2sdr_util litex_m2sdr_rf litex_m2sdr_test

all: $(PROGS)

liblitepcie/liblitepcie.a: liblitepcie/litepcie_dma.o liblitepcie/litepcie_flash.o liblitepcie/litepcie_helpers.o
	ar rcs $@ $+
	ranlib $@

liblitexm2sdr/liblitexm2sdr.a: liblitexm2sdr/litexm2sdr_si5351_i2c.o liblitexm2sdr/litexm2sdr_ad9361_spi.o
	ar rcs $@ $+
	ranlib $@

litex_m2sdr_util: liblitepcie/liblitepcie.a liblitexm2sdr/liblitexm2sdr.a litex_m2sdr_util.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Lliblitexm2sdr -llitepcie -llitexm2sdr

litex_m2sdr_rf: liblitepcie/liblitepcie.a liblitexm2sdr/liblitexm2sdr.a litex_m2sdr_rf.o ad9361/ad9361.o ad9361/ad9361_api.o ad9361/util.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Lliblitexm2sdr -llitepcie -llitexm2sdr

litex_m2sdr_test: liblitepcie/liblitepcie.a litex_m2sdr_test.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie

clean:
	rm -f $(PROGS) *.o *.a *.d *~
	rm -f liblitepcie/*.a liblitepcie/*.o liblitepcie/*.d
	rm -f liblitexm2sdr/*.a liblitexm2sdr/*.o liblitexm2sdr/*.d
	rm -f  ad9361/*.o ad9361/*.d ad9361/*~

%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $<

liblitepcie/%.o: liblitepcie/%.c
	$(CC) -c $(CFLAGS) -o $@ $<

-include $(wildcard *.d)
