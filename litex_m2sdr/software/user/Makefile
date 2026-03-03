PREFIX ?= /usr/local
CFLAGS  = -O3 -Wall -g -I../kernel -Ilibliteeth -Iliblitepcie -Ilibm2sdr -Iad9361 -MMD -fPIC
CFLAGS += -D$(INTERFACE)
CXXFLAGS = $(CFLAGS) -Icimgui -Icimgui/imgui -Icimgui/imgui/backends -DCIMGUI_USE_SDL2 -DCIMGUI_USE_OPENGL3
SDL2_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL2_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
LDFLAGS = -g
CC      = $(CROSS_COMPILE)gcc
CXX     = $(CROSS_COMPILE)g++
AR      = ar

# Backend selection: USE_LITEPCIE (default) or USE_LITEETH
INTERFACE ?= USE_LITEPCIE

# Which binaries to build per interface
ifeq ($(INTERFACE),USE_LITEETH)
  BINARIES := m2sdr_util m2sdr_rf m2sdr_gpio m2sdr_play m2sdr_record m2sdr_sata
else ifeq ($(INTERFACE),USE_LITEPCIE)
  BINARIES := m2sdr_util m2sdr_rf m2sdr_gen m2sdr_play m2sdr_record m2sdr_gpio m2sdr_fm_tx m2sdr_fm_rx m2sdr_sata m2sdr_scan
else
  $(error INTERFACE must be USE_LITEPCIE or USE_LITEETH)
endif

PROGS    = $(BINARIES) libm2sdr/libm2sdr.a ad9361/libad9361_m2sdr.a

all: $(PROGS)

# --- Libraries ---

liblitepcie/liblitepcie.a: liblitepcie/litepcie_dma.o liblitepcie/litepcie_helpers.o
	$(AR) rcs $@ $+
	ranlib $@


libliteeth/libliteeth.a: libliteeth/etherbone.o libliteeth/liteeth_udp.o
	$(AR) rcs $@ $+
	ranlib $@

libm2sdr/libm2sdr.a: libm2sdr/m2sdr_si5351_i2c.o libm2sdr/m2sdr_ad9361_spi.o libm2sdr/m2sdr_flash.o libliteeth/etherbone.o
	$(AR) rcs $@ $+
	ranlib $@

ad9361/libad9361_m2sdr.a: ad9361/ad9361_api.o ad9361/ad9361_conv.o ad9361/ad9361.o ad9361/util.o
	$(AR) rcs $@ $+
	ranlib $@

# --- Binaries ---

# Etherbone-only build
ifeq ($(INTERFACE),USE_LITEETH)
m2sdr_util: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_util.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -llitepcie -lm2sdr -lm

m2sdr_rf: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_rf.o \
	ad9361/ad9361.o ad9361/ad9361_api.o ad9361/ad9361_conv.o ad9361/util.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -llitepcie -lm2sdr

m2sdr_play: liblitepcie/liblitepcie.a libliteeth/libliteeth.a m2sdr_play.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibliteeth -lm -llitepcie -lliteeth

m2sdr_record: liblitepcie/liblitepcie.a libliteeth/libliteeth.a m2sdr_record.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibliteeth -lm -llitepcie -lliteeth

m2sdr_gpio: liblitepcie/liblitepcie.a m2sdr_gpio.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -llitepcie -lm2sdr

m2sdr_sata: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a libliteeth/libliteeth.a m2sdr_sata.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -Llibliteeth -lm -llitepcie -lm2sdr -lliteeth
endif

# PCIe-only build
ifeq ($(INTERFACE),USE_LITEPCIE)
m2sdr_util: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_util.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -llitepcie -lm2sdr -lm

m2sdr_rf: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_rf.o \
	ad9361/ad9361.o ad9361/ad9361_api.o ad9361/ad9361_conv.o ad9361/util.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -llitepcie -lm2sdr

m2sdr_gen: liblitepcie/liblitepcie.a m2sdr_gen.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie

m2sdr_gpio: liblitepcie/liblitepcie.a m2sdr_gpio.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie

m2sdr_play: liblitepcie/liblitepcie.a m2sdr_play.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie

m2sdr_record: liblitepcie/liblitepcie.a m2sdr_record.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie

m2sdr_sata: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a m2sdr_sata.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -llitepcie -lm2sdr

m2sdr_fm_tx: liblitepcie/liblitepcie.a m2sdr_fm_tx.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie -lsndfile -lsamplerate

m2sdr_fm_rx: liblitepcie/liblitepcie.a m2sdr_fm_rx.o
	$(CC) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie -lsndfile -lsamplerate

M2SDR_SCAN_IMGUI_SRCS := \
	cimgui/cimgui.cpp \
	cimgui/imgui/imgui.cpp \
	cimgui/imgui/imgui_draw.cpp \
	cimgui/imgui/imgui_demo.cpp \
	cimgui/imgui/imgui_tables.cpp \
	cimgui/imgui/imgui_widgets.cpp \
	cimgui/imgui/backends/imgui_impl_sdl2.cpp \
	cimgui/imgui/backends/imgui_impl_opengl3.cpp \
	imgui_sdl_gl3_bridge.cpp
M2SDR_SCAN_IMGUI_OBJS := $(M2SDR_SCAN_IMGUI_SRCS:.cpp=.o)

m2sdr_scan: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a ad9361/libad9361_m2sdr.a m2sdr_scan.o simple_fft.o $(M2SDR_SCAN_IMGUI_OBJS)
	$(CXX) $(LDFLAGS) -o $@ m2sdr_scan.o simple_fft.o $(M2SDR_SCAN_IMGUI_OBJS) -Lliblitepcie -Llibm2sdr -Lad9361 -llitepcie -lm2sdr -lad9361_m2sdr -lm $(SDL2_LIBS) -lGL -ldl -pthread -lstdc++
endif

clean:
	rm -f $(PROGS) *.o *.a *.d *~
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

%.o: %.cpp
	$(CXX) -c $(CXXFLAGS) $(SDL2_CFLAGS) -o $@ $<

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
