PREFIX ?= /usr/local
CFLAGS  = -O2 -Wall -g -I../kernel -Ilibliteeth -Iliblitepcie -Ilibm2sdr -Iad9361 -MMD -fPIC
CFLAGS += -D$(INTERFACE)
LDFLAGS = -g
CC      = $(CROSS_COMPILE)gcc
AR      = ar

# Backend selection: USE_LITEPCIE (default) or USE_LITEETH
INTERFACE ?= USE_LITEPCIE

# Which binaries to build per interface
ifeq ($(INTERFACE),USE_LITEETH)
  BINARIES := m2sdr_util m2sdr_rf m2sdr_gpio m2sdr_play m2sdr_record m2sdr_sata
else ifeq ($(INTERFACE),USE_LITEPCIE)
  BINARIES := m2sdr_util m2sdr_rf m2sdr_gen m2sdr_play m2sdr_record m2sdr_gpio m2sdr_fm_tx m2sdr_fm_rx m2sdr_sata
else
  $(error INTERFACE must be USE_LITEPCIE or USE_LITEETH)
endif

LIBM2SDR_SONAME = libm2sdr.so.0
PROGS    = $(BINARIES) libm2sdr/libm2sdr.a libm2sdr/$(LIBM2SDR_SONAME) libm2sdr/libm2sdr.so ad9361/libad9361_m2sdr.a
EXAMPLES = ../doc/libm2sdr/example_sync_rx ../doc/libm2sdr/example_sync_tx

LIBM2SDR_OBJS = \
	libm2sdr/m2sdr_si5351_i2c.o \
	libm2sdr/m2sdr_ad9361_spi.o \
	libm2sdr/m2sdr_flash.o \
	libm2sdr/m2sdr_device.o \
	libm2sdr/m2sdr_stream.o \
	libm2sdr/m2sdr_rf.o \
	libm2sdr/m2sdr_utils.o \
	libm2sdr/m2sdr_hal_litepcie.o \
	libm2sdr/m2sdr_hal_liteeth.o \
	libliteeth/etherbone.o \
	libliteeth/liteeth_udp.o

all: $(PROGS)

examples: libm2sdr/libm2sdr.a $(EXAMPLES)

../doc/libm2sdr/example_sync_rx: ../doc/libm2sdr/example_sync_rx.c libm2sdr/libm2sdr.a
	$(CC) $(LDFLAGS) -o $@ $< -I../kernel -Ilibm2sdr -Llibm2sdr -lm2sdr -Lliblitepcie -llitepcie -lm

../doc/libm2sdr/example_sync_tx: ../doc/libm2sdr/example_sync_tx.c libm2sdr/libm2sdr.a
	$(CC) $(LDFLAGS) -o $@ $< -I../kernel -Ilibm2sdr -Llibm2sdr -lm2sdr -Lliblitepcie -llitepcie -lm

# --- Libraries ---

liblitepcie/liblitepcie.a: liblitepcie/litepcie_dma.o liblitepcie/litepcie_helpers.o
	$(AR) rcs $@ $+
	ranlib $@


libliteeth/libliteeth.a: libliteeth/etherbone.o libliteeth/liteeth_udp.o
	$(AR) rcs $@ $+
	ranlib $@

libm2sdr/libm2sdr.a: $(LIBM2SDR_OBJS)
	$(AR) rcs $@ $+
	ranlib $@

libm2sdr/$(LIBM2SDR_SONAME): $(LIBM2SDR_OBJS)
	$(CC) -shared -Wl,-soname,$(LIBM2SDR_SONAME) -o $@ $^

libm2sdr/libm2sdr.so: libm2sdr/$(LIBM2SDR_SONAME)
	ln -sf $(LIBM2SDR_SONAME) $@

ad9361/libad9361_m2sdr.a: ad9361/ad9361_api.o ad9361/ad9361_conv.o ad9361/ad9361.o ad9361/util.o
	$(AR) rcs $@ $+
	ranlib $@

# --- Binaries ---

# Etherbone-only build
ifeq ($(INTERFACE),USE_LITEETH)
m2sdr_util: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_util.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm2sdr -llitepcie -lm

m2sdr_rf: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_rf.o \
	ad9361/ad9361.o ad9361/ad9361_api.o ad9361/ad9361_conv.o ad9361/util.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm2sdr -llitepcie

m2sdr_play: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a libliteeth/libliteeth.a m2sdr_play.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -Llibliteeth -lm -lm2sdr -llitepcie -lliteeth

m2sdr_record: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a libliteeth/libliteeth.a m2sdr_record.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -Llibliteeth -lm -lm2sdr -llitepcie -lliteeth

m2sdr_gpio: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_gpio.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -lm2sdr -llitepcie

m2sdr_sata: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a libliteeth/libliteeth.a m2sdr_sata.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -Llibliteeth -lm -lm2sdr -llitepcie -lliteeth
endif

# PCIe-only build
ifeq ($(INTERFACE),USE_LITEPCIE)
m2sdr_util: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_util.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm2sdr -llitepcie -lm

m2sdr_rf: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_rf.o \
	ad9361/ad9361.o ad9361/ad9361_api.o ad9361/ad9361_conv.o ad9361/util.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm2sdr -llitepcie

m2sdr_gen: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_gen.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -lm2sdr -llitepcie

m2sdr_gpio: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_gpio.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -lm2sdr -llitepcie

m2sdr_play: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_play.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -lm2sdr -llitepcie

m2sdr_record: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_record.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -lm2sdr -llitepcie

m2sdr_sata: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_sata.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -lm2sdr -llitepcie

m2sdr_fm_tx: liblitepcie/liblitepcie.a m2sdr_fm_tx.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie -lsndfile -lsamplerate

m2sdr_fm_rx: liblitepcie/liblitepcie.a m2sdr_fm_rx.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie -lsndfile -lsamplerate
endif

clean:
	rm -f $(PROGS) *.o *.a *.d *~
	rm -f $(EXAMPLES)
	rm -f liblitepcie/*.a liblitepcie/*.o liblitepcie/*.d
	rm -f libm2sdr/*.a libm2sdr/*.o libm2sdr/*.d
	rm -f libliteeth/*.a libliteeth/*.o libliteeth/*.d
	rm -f ad9361/*.o ad9361/*.d ad9361/*~

# --- Compile rules ---

%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $<

liblitepcie/%.o: liblitepcie/%.c
	$(CC) -c $(CFLAGS) -o $@ $<

libm2sdr/%.o: libm2sdr/%.c
	$(CC) -c $(CFLAGS) -o $@ $<

ad9361/%.o: ad9361/%.c
	$(CC) -c $(CFLAGS) -o $@ $<

install: $(BINARIES)
	install -d $(PREFIX)/bin
	$(foreach bin,$(BINARIES), \
		install -D -m 755 $(bin) $(PREFIX)/bin; \
	)

install_dev: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a libm2sdr/$(LIBM2SDR_SONAME) libm2sdr/libm2sdr.so ad9361/libad9361_m2sdr.a
	install -d $(PREFIX)/lib
	$(foreach lib,$^, \
		install -D -m 755 $(lib) $(PREFIX)/lib; \
	)
	ln -sf $(LIBM2SDR_SONAME) $(PREFIX)/lib/libm2sdr.so
	install -d $(PREFIX)/lib/pkgconfig
	sed -e "s|^prefix=.*|prefix=$(PREFIX)|" libm2sdr/m2sdr.pc > $(PREFIX)/lib/pkgconfig/m2sdr.pc
	chmod 644 $(PREFIX)/lib/pkgconfig/m2sdr.pc
	install -d $(PREFIX)/include/litex_m2sdr
	install -d $(PREFIX)/include/litex_m2sdr/kernel
	cp -f ../kernel/*.h $(PREFIX)/include/litex_m2sdr/kernel
	$(foreach inc,ad9361 liblitepcie libm2sdr libliteeth, \
		install -d $(PREFIX)/include/litex_m2sdr/$(inc); \
		install -m 644 $(inc)/*.h $(PREFIX)/include/litex_m2sdr/$(inc); \
	)
	install -d $(PREFIX)/share/litex_m2sdr/examples
	install -m 644 ../doc/libm2sdr/example_sync_rx.c $(PREFIX)/share/litex_m2sdr/examples
	install -m 644 ../doc/libm2sdr/example_sync_tx.c $(PREFIX)/share/litex_m2sdr/examples

-include $(wildcard *.d)
