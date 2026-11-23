# Report test of the DVB-T2 mode of m2sdr_analyzer, part of test-analyzer (see the Makefile): a
# rotated 32k multiplex and an 8k one with an echo.
.PHONY: test-analyzer-dvbt2
test-analyzer: test-analyzer-dvbt2
test-analyzer-dvbt2: m2sdr_analyzer tests/test_m2sdr_analysis_dvbt2
	for i in 0 1; do \
		./tests/test_m2sdr_analysis_dvbt2 --write $$i tests/analysis_capture_dvbt2_$$i.sc16 && \
		./m2sdr_analyzer --report --json --dvbt2 --file tests/analysis_capture_dvbt2_$$i.sc16 \
			> tests/analysis_capture_dvbt2_$$i.json && \
		./tests/test_m2sdr_analysis_dvbt2 --check $$i tests/analysis_capture_dvbt2_$$i.json || exit 1; \
	done

.PHONY: clean-analyzer-dvbt2
clean: clean-analyzer-dvbt2
clean-analyzer-dvbt2:
	rm -f tests/analysis_capture_dvbt2_*
