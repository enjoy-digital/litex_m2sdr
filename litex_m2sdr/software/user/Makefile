PREFIX ?= /usr/local
CFLAGS  = -O2 -Wall -g -I../kernel -Ilibliteeth -Iliblitepcie -Ilibm2sdr -Iad9361 -MMD -fPIC
LDFLAGS = -g
CC      = $(CROSS_COMPILE)gcc
AR      = ar

BINARIES = m2sdr_util m2sdr_eb_util m2sdr_rf m2sdr_tone m2sdr_play m2sdr_record m2sdr_gpio m2sdr_fm_tx m2sdr_fm_rx
PROGS    = $(BINARIES) libm2sdr/libm2sdr.a ad9361/libad9361_m2sdr.a

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
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -llitepcie -lm2sdr -lm

m2sdr_eb_util: m2sdr_eb_util.o libliteeth/etherbone.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -llitepcie -lm2sdr

m2sdr_rf: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_rf.o \
	ad9361/ad9361.o ad9361/ad9361_api.o ad9361/ad9361_conv.o ad9361/util.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -llitepcie -lm2sdr

m2sdr_tone: liblitepcie/liblitepcie.a m2sdr_tone.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie

m2sdr_gpio: liblitepcie/liblitepcie.a m2sdr_gpio.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie

m2sdr_play: liblitepcie/liblitepcie.a m2sdr_play.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie

m2sdr_record: liblitepcie/liblitepcie.a m2sdr_record.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie

m2sdr_fm_tx: liblitepcie/liblitepcie.a m2sdr_fm_tx.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie -lsndfile -lsamplerate

m2sdr_fm_rx: liblitepcie/liblitepcie.a m2sdr_fm_rx.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie -lsndfile -lsamplerate

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

libm2sdr/%.o: libm2sdr/%.c
	$(CC) -c $(CFLAGS) -DUSE_LITEPCIE -o $@ $<

ad9361/%.o: ad9361/%.c
	$(CC) -c $(CFLAGS) -o $@ $<

install: $(BINARIES)
	install -d $(PREFIX)/bin
	$(foreach bin,$(BINARIES), \
		install -D -m 755 $(bin) $(PREFIX)/bin; \
	)

install_dev: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a ad9361/libad9361_m2sdr.a
	install -d $(PREFIX)/lib
	$(foreach lib,$^, \
		install -D -m 755 $(lib) $(PREFIX)/lib; \
	)
	install -d $(PREFIX)/include/litex_m2sdr
	install -d $(PREFIX)/include/litex_m2sdr/kernel
	cp -f ../kernel/*.h $(PREFIX)/include/litex_m2sdr/kernel
	$(foreach inc,ad9361 liblitepcie libm2sdr libliteeth, \
		install -d $(PREFIX)/include/litex_m2sdr/$(inc); \
		install -m 644 $(inc)/*.h $(PREFIX)/include/litex_m2sdr/$(inc); \
	)

-include $(wildcard *.d)
