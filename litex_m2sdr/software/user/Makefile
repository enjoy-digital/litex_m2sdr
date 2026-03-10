PREFIX ?= /usr/local
SCAN_UI_DIR := scan_ui
SCAN_CIMGUI_DIR := cimgui

CFLAGS  = -O3 -Wall -g -I../kernel -Ilibliteeth -Iliblitepcie -Ilibm2sdr -Iad9361 -I$(SCAN_UI_DIR) -MMD -fPIC
CFLAGS += -D$(INTERFACE)
CFLAGS += $(EXTRA_CFLAGS)
CXXFLAGS = $(CFLAGS) -I. -I$(SCAN_CIMGUI_DIR) -I$(SCAN_CIMGUI_DIR)/imgui -I$(SCAN_CIMGUI_DIR)/imgui/backends -DCIMGUI_USE_SDL2 -DCIMGUI_USE_OPENGL3
SDL2_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL2_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
HAVE_SDL2   := $(shell pkg-config --exists sdl2 2>/dev/null && echo 1 || echo 0)
LDFLAGS = -g
LDFLAGS += $(EXTRA_LDFLAGS)
SANITIZER_FLAGS := $(sort $(filter -fsanitize=% -fno-sanitize-recover=%,$(CFLAGS) $(CXXFLAGS) $(LDFLAGS)))
LDFLAGS += $(SANITIZER_FLAGS)
LINK_CFLAGS := $(filter-out -MMD,$(CFLAGS))
LINK_CXXFLAGS := $(filter-out -MMD,$(CXXFLAGS))
CC      = $(CROSS_COMPILE)gcc
CXX     = $(CROSS_COMPILE)g++
AR      = ar

# Backend selection: USE_LITEPCIE (default) or USE_LITEETH
INTERFACE ?= USE_LITEPCIE

# Which binaries to build per interface
ifeq ($(INTERFACE),USE_LITEETH)
  BINARIES := m2sdr_util m2sdr_rf m2sdr_gpio m2sdr_play m2sdr_record m2sdr_sata m2sdr_selftest
else ifeq ($(INTERFACE),USE_LITEPCIE)
  BINARIES := m2sdr_util m2sdr_rf m2sdr_gen m2sdr_play m2sdr_record m2sdr_gpio m2sdr_fm_tx m2sdr_fm_rx m2sdr_sata m2sdr_selftest
  ifeq ($(HAVE_SDL2),1)
    BINARIES += m2sdr_scan
  else
    $(warning SDL2 not found; skipping m2sdr_scan build)
  endif
else
  $(error INTERFACE must be USE_LITEPCIE or USE_LITEETH)
endif

LIBM2SDR_SONAME = libm2sdr.so.1
PROGS    = $(BINARIES) libm2sdr/libm2sdr.a libm2sdr/$(LIBM2SDR_SONAME) libm2sdr/libm2sdr.so ad9361/libad9361_m2sdr.a
EXAMPLES = ../../doc/libm2sdr/example_sync_rx ../../doc/libm2sdr/example_sync_tx \
           ../../doc/libm2sdr/example_tone_tx ../../doc/libm2sdr/example_rx_n
CLI_OBJS = m2sdr_cli.o
TEST_PROGS = test_libm2sdr

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
	libliteeth/liteeth_udp.o \
	ad9361/ad9361.o \
	ad9361/ad9361_api.o \
	ad9361/ad9361_conv.o \
	ad9361/util.o

all: $(PROGS)

examples: libm2sdr/libm2sdr.a $(EXAMPLES)
test-libm2sdr: $(TEST_PROGS)
	LD_LIBRARY_PATH=libm2sdr:$${LD_LIBRARY_PATH} ./test_libm2sdr

test_libm2sdr: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a test_libm2sdr.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ test_libm2sdr.o -Llibm2sdr -lm2sdr -Lliblitepcie -llitepcie -lm

analyze-libm2sdr:
	cppcheck --enable=warning,style,performance,portability --error-exitcode=1 \
		--suppress=constParameterPointer \
		--suppress=variableScope \
		--suppress=unreadVariable \
		--suppress=badBitmaskCheck \
		-I../kernel -Ilibliteeth -Iliblitepcie -Ilibm2sdr -Iad9361 \
		libm2sdr

../../doc/libm2sdr/example_sync_rx: ../../doc/libm2sdr/example_sync_rx.c liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $< -I../kernel -Ilibm2sdr -Llibm2sdr -lm2sdr -Lliblitepcie -llitepcie -lm

../../doc/libm2sdr/example_sync_tx: ../../doc/libm2sdr/example_sync_tx.c liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $< -I../kernel -Ilibm2sdr -Llibm2sdr -lm2sdr -Lliblitepcie -llitepcie -lm

../../doc/libm2sdr/example_tone_tx: ../../doc/libm2sdr/example_tone_tx.c liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $< -I../kernel -Ilibm2sdr -Llibm2sdr -lm2sdr -Lliblitepcie -llitepcie -lm

../../doc/libm2sdr/example_rx_n: ../../doc/libm2sdr/example_rx_n.c liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $< -I../kernel -Ilibm2sdr -Llibm2sdr -lm2sdr -Lliblitepcie -llitepcie -lm

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
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -shared -Wl,-soname,$(LIBM2SDR_SONAME) -Wl,-Bsymbolic -o $@ $^

libm2sdr/libm2sdr.so: libm2sdr/$(LIBM2SDR_SONAME)
	ln -sf $(LIBM2SDR_SONAME) $@

ad9361/libad9361_m2sdr.a: ad9361/ad9361_api.o ad9361/ad9361_conv.o ad9361/ad9361.o ad9361/util.o
	$(AR) rcs $@ $+
	ranlib $@

# --- Binaries ---

# Etherbone-only build
ifeq ($(INTERFACE),USE_LITEETH)
m2sdr_util: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a $(CLI_OBJS) m2sdr_util.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm2sdr -llitepcie -lm

m2sdr_rf: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a $(CLI_OBJS) m2sdr_rf.o \
	ad9361/ad9361.o ad9361/ad9361_api.o ad9361/ad9361_conv.o ad9361/util.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm2sdr -llitepcie

m2sdr_play: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a libliteeth/libliteeth.a $(CLI_OBJS) m2sdr_play.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -Llibliteeth -lm -lm2sdr -llitepcie -lliteeth

m2sdr_record: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a libliteeth/libliteeth.a $(CLI_OBJS) m2sdr_record.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -Llibliteeth -lm -lm2sdr -llitepcie -lliteeth

m2sdr_gpio: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a $(CLI_OBJS) m2sdr_gpio.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -lm2sdr -llitepcie

m2sdr_sata: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a libliteeth/libliteeth.a $(CLI_OBJS) m2sdr_sata.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -Llibliteeth -lm -lm2sdr -llitepcie -lliteeth

m2sdr_selftest: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a $(CLI_OBJS) m2sdr_selftest.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -lm2sdr -llitepcie

endif

# PCIe-only build
ifeq ($(INTERFACE),USE_LITEPCIE)
m2sdr_util: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a $(CLI_OBJS) m2sdr_util.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm2sdr -llitepcie -lm

m2sdr_rf: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a $(CLI_OBJS) m2sdr_rf.o \
	ad9361/ad9361.o ad9361/ad9361_api.o ad9361/ad9361_conv.o ad9361/util.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm2sdr -llitepcie

m2sdr_gen: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a $(CLI_OBJS) m2sdr_gen.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -lm2sdr -llitepcie

m2sdr_gpio: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a $(CLI_OBJS) m2sdr_gpio.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -lm2sdr -llitepcie

m2sdr_play: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a $(CLI_OBJS) m2sdr_play.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -lm2sdr -llitepcie

m2sdr_record: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a $(CLI_OBJS) m2sdr_record.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -lm2sdr -llitepcie

m2sdr_sata: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a $(CLI_OBJS) m2sdr_sata.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -lm2sdr -llitepcie

m2sdr_selftest: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a $(CLI_OBJS) m2sdr_selftest.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -Llibm2sdr -lm -lm2sdr -llitepcie

m2sdr_fm_tx: liblitepcie/liblitepcie.a m2sdr_fm_tx.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie -lsndfile -lsamplerate

m2sdr_fm_rx: liblitepcie/liblitepcie.a m2sdr_fm_rx.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $^ -Lliblitepcie -lm -llitepcie -lsndfile -lsamplerate

M2SDR_SCAN_IMGUI_SRCS := \
	$(SCAN_CIMGUI_DIR)/cimgui.cpp \
	$(SCAN_CIMGUI_DIR)/imgui/imgui.cpp \
	$(SCAN_CIMGUI_DIR)/imgui/imgui_draw.cpp \
	$(SCAN_CIMGUI_DIR)/imgui/imgui_demo.cpp \
	$(SCAN_CIMGUI_DIR)/imgui/imgui_tables.cpp \
	$(SCAN_CIMGUI_DIR)/imgui/imgui_widgets.cpp \
	$(SCAN_CIMGUI_DIR)/imgui/backends/imgui_impl_sdl2.cpp \
	$(SCAN_CIMGUI_DIR)/imgui/backends/imgui_impl_opengl3.cpp \
	$(SCAN_UI_DIR)/imgui_sdl_gl3_bridge.cpp
M2SDR_SCAN_IMGUI_OBJS := $(M2SDR_SCAN_IMGUI_SRCS:.cpp=.o)
M2SDR_SCAN_SRCS := m2sdr_scan.c kissfft/kiss_fft.c
M2SDR_SCAN_OBJS := $(M2SDR_SCAN_SRCS:.c=.o)

m2sdr_scan: liblitepcie/liblitepcie.a libm2sdr/libm2sdr.a ad9361/libad9361_m2sdr.a $(CLI_OBJS) $(M2SDR_SCAN_OBJS) $(M2SDR_SCAN_IMGUI_OBJS)
	$(CXX) $(LINK_CXXFLAGS) $(LDFLAGS) -o $@ $(CLI_OBJS) $(M2SDR_SCAN_OBJS) $(M2SDR_SCAN_IMGUI_OBJS) -Lliblitepcie -Llibm2sdr -Lad9361 -llitepcie -lm2sdr -lad9361_m2sdr -lm $(SDL2_LIBS) -lGL -ldl -pthread -lstdc++
endif

clean:
	rm -f $(PROGS) *.o *.a *.d *~
	rm -f $(EXAMPLES)
	rm -f liblitepcie/*.a liblitepcie/*.o liblitepcie/*.d
	rm -f libm2sdr/*.a libm2sdr/*.o libm2sdr/*.d
	rm -f libliteeth/*.a libliteeth/*.o libliteeth/*.d
	rm -f ad9361/*.o ad9361/*.d ad9361/*~
	rm -f kissfft/*.o kissfft/*.d
	rm -f $(SCAN_UI_DIR)/*.o $(SCAN_UI_DIR)/*.d
	rm -f $(SCAN_CIMGUI_DIR)/*.o $(SCAN_CIMGUI_DIR)/*.d
	rm -f $(SCAN_CIMGUI_DIR)/imgui/*.o $(SCAN_CIMGUI_DIR)/imgui/*.d
	rm -f $(SCAN_CIMGUI_DIR)/imgui/backends/*.o $(SCAN_CIMGUI_DIR)/imgui/backends/*.d

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
	@for ex in example_sync_rx example_sync_tx example_tone_tx example_rx_n; do \
		if [ -f ../doc/libm2sdr/$$ex.c ]; then \
			install -m 644 ../doc/libm2sdr/$$ex.c $(PREFIX)/share/litex_m2sdr/examples; \
		else \
			echo "Skipping missing example: $$ex.c"; \
		fi; \
	done

check:
	$(MAKE) clean all
	$(MAKE) examples
	cmake -S ../soapysdr -B ../soapysdr/build
	cmake --build ../soapysdr/build

-include $(wildcard *.d)
