PREFIX ?= /usr/local
SCAN_UI_DIR := scan_ui
SCAN_CIMGUI_DIR := cimgui
M2SDR_VERSION := 1.0.0

# Backend selection: USE_RUNTIME (default), USE_LITEPCIE, or USE_LITEETH.
# USE_RUNTIME builds one binary set that can open either pcie: or eth: devices.
INTERFACE ?= USE_RUNTIME

ifeq ($(INTERFACE),USE_RUNTIME)
  TRANSPORT_CFLAGS := -DUSE_LITEPCIE -DUSE_LITEETH -DM2SDR_DEFAULT_TRANSPORT_LITEPCIE
else ifeq ($(INTERFACE),USE_LITEPCIE)
  TRANSPORT_CFLAGS := -DUSE_LITEPCIE -DUSE_LITEETH -DM2SDR_DEFAULT_TRANSPORT_LITEPCIE
else ifeq ($(INTERFACE),USE_LITEETH)
  TRANSPORT_CFLAGS := -DUSE_LITEPCIE -DUSE_LITEETH -DM2SDR_DEFAULT_TRANSPORT_LITEETH
else
  $(error INTERFACE must be USE_RUNTIME, USE_LITEPCIE, or USE_LITEETH)
endif

CFLAGS  = -O3 -Wall -g -I../kernel -Ilibliteeth -Iliblitepcie -Ilibm2sdr -Iad9361 -I$(SCAN_UI_DIR) -MMD -fPIC
CFLAGS += $(TRANSPORT_CFLAGS)
CFLAGS += $(EXTRA_CFLAGS)
CXXFLAGS = $(CFLAGS) -I. -I$(SCAN_CIMGUI_DIR) -I$(SCAN_CIMGUI_DIR)/imgui -I$(SCAN_CIMGUI_DIR)/imgui/backends -DCIMGUI_USE_SDL2 -DCIMGUI_USE_OPENGL3
SDL2_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL2_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
HAVE_SDL2   := $(shell pkg-config --exists sdl2 2>/dev/null && echo 1 || echo 0)
PNG_CFLAGS  := $(shell pkg-config --cflags libpng 2>/dev/null)
PNG_LIBS    := $(shell pkg-config --libs libpng 2>/dev/null)
HAVE_PNG    := $(shell pkg-config --exists libpng 2>/dev/null && echo 1 || echo 0)
HAVE_CIMGUI := $(shell test -f $(SCAN_CIMGUI_DIR)/cimgui.h && test -f $(SCAN_CIMGUI_DIR)/imgui/imgui.cpp && echo 1 || echo 0)
SNDFILE_CFLAGS    := $(shell pkg-config --cflags sndfile 2>/dev/null)
SNDFILE_LIBS      := $(shell pkg-config --libs sndfile 2>/dev/null || echo -lsndfile)
SAMPLERATE_CFLAGS := $(shell pkg-config --cflags samplerate 2>/dev/null)
SAMPLERATE_LIBS   := $(shell pkg-config --libs samplerate 2>/dev/null || echo -lsamplerate)
HAVE_SNDFILE      := $(shell pkg-config --exists sndfile 2>/dev/null && echo 1 || echo 0)
HAVE_SAMPLERATE   := $(shell pkg-config --exists samplerate 2>/dev/null && echo 1 || echo 0)
LDFLAGS = -g
LDFLAGS += $(EXTRA_LDFLAGS)
SANITIZER_FLAGS := $(sort $(filter -fsanitize=% -fno-sanitize-recover=%,$(CFLAGS) $(CXXFLAGS) $(LDFLAGS)))
LDFLAGS += $(SANITIZER_FLAGS)
LINK_CFLAGS := $(filter-out -MMD,$(CFLAGS))
LINK_CXXFLAGS := $(filter-out -MMD,$(CXXFLAGS))
BUILD_FLAGS_FILE := .build-flags
BUILD_FLAGS_CONTENT := BUILD_FLAGS_SCHEMA=2\nINTERFACE=$(INTERFACE)\nCFLAGS=$(CFLAGS)\nCXXFLAGS=$(CXXFLAGS)\nLDFLAGS=$(LDFLAGS)
CC      = $(CROSS_COMPILE)gcc
CXX     = $(CROSS_COMPILE)g++
AR      = ar
.DEFAULT_GOAL := all
FORCE:
.PHONY: FORCE all core-tools core-tests examples test-libm2sdr test-sigmf test-sigmf-smoke test-sigmf-cli clean install install_dev check analyze-libm2sdr

ALL_BINARIES := m2sdr_util m2sdr_rf m2sdr_gpio m2sdr_play m2sdr_record m2sdr_sigmf m2sdr_sata \
                m2sdr_gen m2sdr_fm_tx m2sdr_fm_rx m2sdr_check m2sdr_scan

# Which core binaries to build per interface.
ifeq ($(INTERFACE),USE_LITEETH)
  CORE_BINARIES := m2sdr_util m2sdr_rf m2sdr_gpio m2sdr_play m2sdr_record m2sdr_sigmf m2sdr_sata
else ifneq ($(filter $(INTERFACE),USE_RUNTIME USE_LITEPCIE),)
  CORE_BINARIES := m2sdr_util m2sdr_rf m2sdr_gen m2sdr_play m2sdr_record m2sdr_sigmf m2sdr_gpio m2sdr_sata
endif

BINARIES := $(CORE_BINARIES)

ifeq ($(and $(filter 1,$(HAVE_SNDFILE)),$(filter 1,$(HAVE_SAMPLERATE))),1)
  BINARIES += m2sdr_fm_tx m2sdr_fm_rx
else
  ifneq ($(HAVE_SNDFILE),1)
    $(warning sndfile pkg-config metadata not found; skipping FM audio tools)
  endif
  ifneq ($(HAVE_SAMPLERATE),1)
    $(warning samplerate pkg-config metadata not found; skipping FM audio tools)
  endif
endif

ifeq ($(and $(filter 1,$(HAVE_SDL2)),$(filter 1,$(HAVE_CIMGUI))),1)
  BINARIES += m2sdr_check
  ifneq ($(INTERFACE),USE_LITEETH)
    ifeq ($(HAVE_PNG),1)
      BINARIES += m2sdr_scan
    else
      $(warning libpng pkg-config metadata not found; skipping m2sdr_scan)
    endif
  endif
else
  ifneq ($(HAVE_SDL2),1)
    $(warning SDL2 not found; skipping SDL2-based tools)
  endif
  ifneq ($(HAVE_CIMGUI),1)
    $(warning cimgui sources not found in software/user/cimgui; skipping SDL2-based tools)
  endif
endif

LIBM2SDR_SONAME = libm2sdr.so.1
LIBM2SDR_STATIC = libm2sdr/libm2sdr.a
LIBM2SDR_SHARED = libm2sdr/$(LIBM2SDR_SONAME) libm2sdr/libm2sdr.so
PROGS    = $(BINARIES) $(LIBM2SDR_STATIC) $(LIBM2SDR_SHARED)
EXAMPLES = ../../doc/libm2sdr/example_sync_rx ../../doc/libm2sdr/example_sync_tx \
           ../../doc/libm2sdr/example_tone_tx ../../doc/libm2sdr/example_rx_n
CLI_OBJS = m2sdr_cli.o
TOOL_OBJS = m2sdr_tool.o
TEST_PROGS = tests/test_libm2sdr tests/test_m2sdr_sigmf tests/test_sigmf_capture_smoke tests/test_sigmf_cli

LIBM2SDR_OBJS = \
	libm2sdr/m2sdr_si5351_i2c.o \
	libm2sdr/m2sdr_ad9361_spi.o \
	libm2sdr/m2sdr_flash.o \
	libm2sdr/m2sdr_device.o \
	libm2sdr/m2sdr_stream.o \
	libm2sdr/m2sdr_rf.o \
	libm2sdr/m2sdr_utils.o \
	libm2sdr/m2sdr_json.o \
	libm2sdr/m2sdr_sigmf.o \
	libm2sdr/m2sdr_hal_litepcie.o \
	libm2sdr/m2sdr_hal_liteeth.o \
	liblitepcie/litepcie_dma.o \
	liblitepcie/litepcie_helpers.o \
	libliteeth/etherbone.o \
	libliteeth/liteeth_udp.o \
	ad9361/ad9361.o \
	ad9361/ad9361_api.o \
	ad9361/ad9361_conv.o \
	ad9361/util.o

M2SDR_IMGUI_SRCS := \
	$(SCAN_CIMGUI_DIR)/cimgui.cpp \
	$(SCAN_CIMGUI_DIR)/imgui/imgui.cpp \
	$(SCAN_CIMGUI_DIR)/imgui/imgui_draw.cpp \
	$(SCAN_CIMGUI_DIR)/imgui/imgui_demo.cpp \
	$(SCAN_CIMGUI_DIR)/imgui/imgui_tables.cpp \
	$(SCAN_CIMGUI_DIR)/imgui/imgui_widgets.cpp \
	$(SCAN_CIMGUI_DIR)/imgui/backends/imgui_impl_sdl2.cpp \
	$(SCAN_CIMGUI_DIR)/imgui/backends/imgui_impl_opengl3.cpp \
	$(SCAN_UI_DIR)/imgui_sdl_gl3_bridge.cpp
M2SDR_IMGUI_OBJS := $(M2SDR_IMGUI_SRCS:.cpp=.o)

all: $(PROGS)
core-tools: $(CORE_BINARIES) $(LIBM2SDR_STATIC) $(LIBM2SDR_SHARED)
core-tests: core-tools test-libm2sdr test-sigmf test-sigmf-smoke test-sigmf-cli

examples: $(LIBM2SDR_STATIC) $(EXAMPLES)
test-libm2sdr: $(TEST_PROGS)
	LD_LIBRARY_PATH=libm2sdr:$${LD_LIBRARY_PATH} ./tests/test_libm2sdr

test-sigmf: tests/test_m2sdr_sigmf
	./tests/test_m2sdr_sigmf

test-sigmf-smoke: tests/test_sigmf_capture_smoke
	./tests/test_sigmf_capture_smoke

test-sigmf-cli: tests/test_sigmf_cli m2sdr_sigmf
	./tests/test_sigmf_cli

tests/test_libm2sdr: $(LIBM2SDR_STATIC) $(CLI_OBJS) $(TOOL_OBJS) tests/test_libm2sdr.o | $(BUILD_FLAGS_FILE)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $(CLI_OBJS) $(TOOL_OBJS) tests/test_libm2sdr.o $(LIBM2SDR_STATIC) -lm

tests/test_m2sdr_sigmf: $(LIBM2SDR_STATIC) tests/test_m2sdr_sigmf.o | $(BUILD_FLAGS_FILE)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ tests/test_m2sdr_sigmf.o $(LIBM2SDR_STATIC) -lm

tests/test_sigmf_capture_smoke: $(LIBM2SDR_STATIC) tests/test_sigmf_capture_smoke.o | $(BUILD_FLAGS_FILE)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ tests/test_sigmf_capture_smoke.o $(LIBM2SDR_STATIC) -lm

tests/test_sigmf_cli: tests/test_sigmf_cli.o | $(BUILD_FLAGS_FILE)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ tests/test_sigmf_cli.o -lm

analyze-libm2sdr:
	cppcheck --enable=warning,style,performance,portability --error-exitcode=1 \
		--suppress=constParameter \
		--suppress=constParameterPointer \
		--suppress=variableScope \
		--suppress=unreadVariable \
		--suppress=badBitmaskCheck \
		-I../kernel -Ilibliteeth -Iliblitepcie -Ilibm2sdr -Iad9361 \
		libm2sdr

../../doc/libm2sdr/example_sync_rx: ../../doc/libm2sdr/example_sync_rx.c $(LIBM2SDR_STATIC) | $(BUILD_FLAGS_FILE)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $< -I../kernel -Ilibm2sdr $(LIBM2SDR_STATIC) -lm

../../doc/libm2sdr/example_sync_tx: ../../doc/libm2sdr/example_sync_tx.c $(LIBM2SDR_STATIC) | $(BUILD_FLAGS_FILE)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $< -I../kernel -Ilibm2sdr $(LIBM2SDR_STATIC) -lm

../../doc/libm2sdr/example_tone_tx: ../../doc/libm2sdr/example_tone_tx.c $(LIBM2SDR_STATIC) | $(BUILD_FLAGS_FILE)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $< -I../kernel -Ilibm2sdr $(LIBM2SDR_STATIC) -lm

../../doc/libm2sdr/example_rx_n: ../../doc/libm2sdr/example_rx_n.c $(LIBM2SDR_STATIC) | $(BUILD_FLAGS_FILE)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $< -I../kernel -Ilibm2sdr $(LIBM2SDR_STATIC) -lm

# --- Libraries ---

liblitepcie/liblitepcie.a: $(BUILD_FLAGS_FILE) liblitepcie/litepcie_dma.o liblitepcie/litepcie_helpers.o
	$(AR) rcs $@ liblitepcie/litepcie_dma.o liblitepcie/litepcie_helpers.o
	ranlib $@


libliteeth/libliteeth.a: $(BUILD_FLAGS_FILE) libliteeth/etherbone.o libliteeth/liteeth_udp.o
	$(AR) rcs $@ libliteeth/etherbone.o libliteeth/liteeth_udp.o
	ranlib $@

libm2sdr/libm2sdr.a: $(BUILD_FLAGS_FILE) $(LIBM2SDR_OBJS)
	$(AR) rcs $@ $(LIBM2SDR_OBJS)
	ranlib $@

libm2sdr/$(LIBM2SDR_SONAME): $(BUILD_FLAGS_FILE) $(LIBM2SDR_OBJS)
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -shared -Wl,-soname,$(LIBM2SDR_SONAME) -Wl,-Bsymbolic -o $@ $(LIBM2SDR_OBJS)

libm2sdr/libm2sdr.so: libm2sdr/$(LIBM2SDR_SONAME)
	ln -sf $(LIBM2SDR_SONAME) $@

ad9361/libad9361_m2sdr.a: $(BUILD_FLAGS_FILE) ad9361/ad9361_api.o ad9361/ad9361_conv.o ad9361/ad9361.o ad9361/util.o
	$(AR) rcs $@ ad9361/ad9361_api.o ad9361/ad9361_conv.o ad9361/ad9361.o ad9361/util.o
	ranlib $@

# --- Binaries ---

M2SDR_LINK_LIBS := $(LIBM2SDR_STATIC) -lm

m2sdr_util: $(LIBM2SDR_STATIC) $(CLI_OBJS) m2sdr_util.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $(CLI_OBJS) m2sdr_util.o $(M2SDR_LINK_LIBS)

m2sdr_rf: $(LIBM2SDR_STATIC) $(CLI_OBJS) m2sdr_rf.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $(CLI_OBJS) m2sdr_rf.o $(M2SDR_LINK_LIBS)

m2sdr_play: $(LIBM2SDR_STATIC) $(CLI_OBJS) $(TOOL_OBJS) m2sdr_play.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $(CLI_OBJS) $(TOOL_OBJS) m2sdr_play.o $(M2SDR_LINK_LIBS)

m2sdr_record: $(LIBM2SDR_STATIC) $(CLI_OBJS) $(TOOL_OBJS) m2sdr_record.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $(CLI_OBJS) $(TOOL_OBJS) m2sdr_record.o $(M2SDR_LINK_LIBS)

m2sdr_sigmf: $(LIBM2SDR_STATIC) m2sdr_sigmf.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ m2sdr_sigmf.o $(M2SDR_LINK_LIBS)

m2sdr_gpio: $(LIBM2SDR_STATIC) $(CLI_OBJS) m2sdr_gpio.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $(CLI_OBJS) m2sdr_gpio.o $(M2SDR_LINK_LIBS)

m2sdr_sata: $(LIBM2SDR_STATIC) $(CLI_OBJS) m2sdr_sata.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $(CLI_OBJS) m2sdr_sata.o $(M2SDR_LINK_LIBS)

ifeq ($(HAVE_SDL2),1)
m2sdr_check: $(LIBM2SDR_STATIC) kissfft/kiss_fft.o $(CLI_OBJS) $(TOOL_OBJS) m2sdr_check.o $(M2SDR_IMGUI_OBJS)
	$(CXX) $(LINK_CXXFLAGS) $(LDFLAGS) -o $@ kissfft/kiss_fft.o $(CLI_OBJS) $(TOOL_OBJS) m2sdr_check.o $(M2SDR_IMGUI_OBJS) $(M2SDR_LINK_LIBS) $(SDL2_LIBS) -lGL -ldl -pthread -lstdc++
endif

m2sdr_fm_tx: $(CLI_OBJS) m2sdr_fm_tx.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $(CLI_OBJS) m2sdr_fm_tx.o -lm $(SNDFILE_LIBS) $(SAMPLERATE_LIBS)

m2sdr_fm_rx: $(CLI_OBJS) m2sdr_fm_rx.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $(CLI_OBJS) m2sdr_fm_rx.o -lm $(SNDFILE_LIBS) $(SAMPLERATE_LIBS)

m2sdr_fm_tx.o: m2sdr_fm_tx.c $(BUILD_FLAGS_FILE)
	$(CC) -c $(CFLAGS) $(SNDFILE_CFLAGS) $(SAMPLERATE_CFLAGS) -o $@ $<

m2sdr_fm_rx.o: m2sdr_fm_rx.c $(BUILD_FLAGS_FILE)
	$(CC) -c $(CFLAGS) $(SNDFILE_CFLAGS) $(SAMPLERATE_CFLAGS) -o $@ $<

# PCIe-oriented tools.
ifneq ($(INTERFACE),USE_LITEETH)
m2sdr_gen: $(LIBM2SDR_STATIC) $(CLI_OBJS) $(TOOL_OBJS) m2sdr_gen.o kissfft/kiss_fft.o
	$(CC) $(LINK_CFLAGS) $(LDFLAGS) -o $@ $(CLI_OBJS) $(TOOL_OBJS) m2sdr_gen.o kissfft/kiss_fft.o $(M2SDR_LINK_LIBS)

M2SDR_SCAN_SRCS := m2sdr_scan.c kissfft/kiss_fft.c
M2SDR_SCAN_OBJS := $(M2SDR_SCAN_SRCS:.c=.o)

m2sdr_scan: $(LIBM2SDR_STATIC) $(CLI_OBJS) $(M2SDR_SCAN_OBJS) $(M2SDR_IMGUI_OBJS)
	$(CXX) $(LINK_CXXFLAGS) $(LDFLAGS) -o $@ $(CLI_OBJS) $(M2SDR_SCAN_OBJS) $(M2SDR_IMGUI_OBJS) $(M2SDR_LINK_LIBS) $(SDL2_LIBS) $(PNG_LIBS) -lGL -ldl -pthread -lstdc++
endif

clean:
	rm -f $(PROGS) $(ALL_BINARIES) *.o *.a *.d *~
	rm -f $(EXAMPLES)
	rm -f .build-flags .build-flags-*
	rm -f liblitepcie/*.a liblitepcie/*.o liblitepcie/*.d
	rm -f libm2sdr/*.a libm2sdr/*.o libm2sdr/*.d
	rm -f tests/*.o tests/*.d tests/test_libm2sdr tests/test_m2sdr_sigmf tests/test_sigmf_capture_smoke tests/test_sigmf_cli
	rm -f libliteeth/*.a libliteeth/*.o libliteeth/*.d
	rm -f ad9361/*.o ad9361/*.d ad9361/*~
	rm -f kissfft/*.o kissfft/*.d
	rm -f $(SCAN_UI_DIR)/*.o $(SCAN_UI_DIR)/*.d
	rm -f $(SCAN_CIMGUI_DIR)/*.o $(SCAN_CIMGUI_DIR)/*.d
	rm -f $(SCAN_CIMGUI_DIR)/imgui/*.o $(SCAN_CIMGUI_DIR)/imgui/*.d
	rm -f $(SCAN_CIMGUI_DIR)/imgui/backends/*.o $(SCAN_CIMGUI_DIR)/imgui/backends/*.d

# --- Compile rules ---

$(BUILD_FLAGS_FILE): FORCE
	@printf '%b\n' "$(BUILD_FLAGS_CONTENT)" > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then \
		rm -f $(ALL_BINARIES); \
		rm -f *.o *.a *.d; \
		rm -f liblitepcie/*.a liblitepcie/*.o liblitepcie/*.d; \
		rm -f libm2sdr/*.a libm2sdr/*.o libm2sdr/*.d; \
		rm -f tests/*.o tests/*.d tests/test_libm2sdr tests/test_m2sdr_sigmf tests/test_sigmf_capture_smoke tests/test_sigmf_cli; \
		rm -f libliteeth/*.a libliteeth/*.o libliteeth/*.d; \
		rm -f ad9361/*.o ad9361/*.d; \
		rm -f kissfft/*.o kissfft/*.d; \
		rm -f $(SCAN_UI_DIR)/*.o $(SCAN_UI_DIR)/*.d; \
		rm -f $(SCAN_CIMGUI_DIR)/*.o $(SCAN_CIMGUI_DIR)/*.d; \
		rm -f $(SCAN_CIMGUI_DIR)/imgui/*.o $(SCAN_CIMGUI_DIR)/imgui/*.d; \
		rm -f $(SCAN_CIMGUI_DIR)/imgui/backends/*.o $(SCAN_CIMGUI_DIR)/imgui/backends/*.d; \
		mv $@.tmp $@; \
	else \
		rm -f $@.tmp; \
	fi

%.o: %.c $(BUILD_FLAGS_FILE)
	$(CC) -c $(CFLAGS) $(PNG_CFLAGS) -o $@ $<

liblitepcie/%.o: liblitepcie/%.c $(BUILD_FLAGS_FILE)
	$(CC) -c $(CFLAGS) $(PNG_CFLAGS) -o $@ $<

tests/%.o: tests/%.c $(BUILD_FLAGS_FILE)
	$(CC) -c $(CFLAGS) $(PNG_CFLAGS) -o $@ $<

libm2sdr/%.o: libm2sdr/%.c $(BUILD_FLAGS_FILE)
	$(CC) -c $(CFLAGS) $(PNG_CFLAGS) -o $@ $<

ad9361/%.o: ad9361/%.c $(BUILD_FLAGS_FILE)
	$(CC) -c $(CFLAGS) $(PNG_CFLAGS) -o $@ $<

%.o: %.cpp $(BUILD_FLAGS_FILE)
	$(CXX) -c $(CXXFLAGS) $(SDL2_CFLAGS) -o $@ $<

install: $(BINARIES)
	install -d $(PREFIX)/bin
	$(foreach bin,$(BINARIES), \
		install -D -m 755 $(bin) $(PREFIX)/bin; \
	)

install_dev: $(LIBM2SDR_STATIC) $(LIBM2SDR_SHARED)
	install -d $(PREFIX)/lib
	$(foreach lib,$^, \
		install -D -m 755 $(lib) $(PREFIX)/lib; \
	)
	ln -sf $(LIBM2SDR_SONAME) $(PREFIX)/lib/libm2sdr.so
	install -d $(PREFIX)/lib/pkgconfig
	sed -e "s|^prefix=.*|prefix=$(PREFIX)|" libm2sdr/m2sdr.pc > $(PREFIX)/lib/pkgconfig/m2sdr.pc
	chmod 644 $(PREFIX)/lib/pkgconfig/m2sdr.pc
	install -d $(PREFIX)/lib/cmake/m2sdr
	sed -e "s|@M2SDR_VERSION@|$(M2SDR_VERSION)|g" libm2sdr/m2sdrConfig.cmake.in > $(PREFIX)/lib/cmake/m2sdr/m2sdrConfig.cmake
	sed -e "s|@M2SDR_VERSION@|$(M2SDR_VERSION)|g" libm2sdr/m2sdrConfigVersion.cmake.in > $(PREFIX)/lib/cmake/m2sdr/m2sdrConfigVersion.cmake
	chmod 644 $(PREFIX)/lib/cmake/m2sdr/m2sdrConfig.cmake $(PREFIX)/lib/cmake/m2sdr/m2sdrConfigVersion.cmake
	install -d $(PREFIX)/include/litex_m2sdr
	install -d $(PREFIX)/include/litex_m2sdr/kernel
	cp -f ../kernel/*.h $(PREFIX)/include/litex_m2sdr/kernel
	install -d $(PREFIX)/include/litex_m2sdr/libm2sdr
	install -m 644 libm2sdr/m2sdr.h $(PREFIX)/include/litex_m2sdr/libm2sdr
	install -m 644 libm2sdr/m2sdr_sigmf.h $(PREFIX)/include/litex_m2sdr/libm2sdr
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

-include $(wildcard *.d tests/*.d libm2sdr/*.d)
