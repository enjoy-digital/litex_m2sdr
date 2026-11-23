# Report test of the FM mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-fm
test-analyzer: test-analyzer-fm
test-analyzer-fm: m2sdr_analyzer tests/test_m2sdr_analysis_fm
	./tests/test_m2sdr_analysis_fm --write-fm tests/analysis_capture_fm.sc16
	./m2sdr_analyzer --report --json --fm --fm-tone 1000 --file tests/analysis_capture_fm.sc16 --freq 98e6 \
		> tests/analysis_capture_fm.json
	./tests/test_m2sdr_analysis_fm --check-fm tests/analysis_capture_fm.json

.PHONY: clean-analyzer-fm
clean: clean-analyzer-fm
clean-analyzer-fm:
	rm -f tests/analysis_capture_fm.sc16 tests/analysis_capture_fm.json
