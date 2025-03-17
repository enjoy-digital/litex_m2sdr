CFLAGS  = -O2 -Wall -g -I../kernel -Ilibliteeth -Iliblitepcie -Ilibm2sdr -Iad9361 -MMD -fPIC
LDFLAGS = -g
CC      = $(CROSS_COMPILE)gcc
AR      = ar

PROGS=m2sdr_util m2sdr_rf m2sdr_tone m2sdr_play m2sdr_record libm2sdr/libm2sdr.a ad9361/libad9361_m2sdr.a

all: $(PROGS)

liblitepcie/liblitepcie.a: liblitepcie/litepcie_dma.o liblitepcie/litepcie_flash.o liblitepcie/litepcie_helpers.o
	ar rcs $@ $+
	ranlib $@

libm2sdr/libm2sdr.a: libm2sdr/m2sdr_si5351_i2c.o libm2sdr/m2sdr_ad9361_spi.o libliteeth/etherbone.o
	ar rcs $@ $+
	ranlib $@

ad9361/libad9361_m2sdr.a: ad9361/ad9361_api.o ad9361/ad9361_conv.o ad9361/ad9361.o ad9361/util.o
	ar rcs $@ $+
	ranlib $@

m2sdr_util: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_util.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -llitepcie -lm2sdr

m2sdr_rf: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_rf.o \
	ad9361/ad9361.o ad9361/ad9361_api.o ad9361/ad9361_conv.o ad9361/util.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -llitepcie -lm2sdr

m2sdr_tone: liblitepcie/liblitepcie.a m2sdr_tone.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie

m2sdr_play: liblitepcie/liblitepcie.a m2sdr_play.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie

m2sdr_record: liblitepcie/liblitepcie.a m2sdr_record.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie

clean:
	rm -f $(PROGS) *.o *.a *.d *~
	rm -f liblitepcie/*.a liblitepcie/*.o liblitepcie/*.d
	rm -f libm2sdr/*.a libm2sdr/*.o libm2sdr/*.d
	rm -f libliteeth/*.a libliteeth/*.o libliteeth/*.d
	rm -f  ad9361/*.o ad9361/*.d ad9361/*~

%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $<

liblitepcie/%.o: liblitepcie/%.c
	$(CC) -c $(CFLAGS) -o $@ $<

ad9361/%.o: ad9361/%.c
	$(CC) -c $(CFLAGS) -o $@ $<

-include $(wildcard *.d)
