# Report test of the LTE 2x2 mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-mimo
test-analyzer: test-analyzer-mimo
test-analyzer-mimo: m2sdr_analyzer tests/test_m2sdr_analysis_dual
	./tests/test_m2sdr_analysis_dual --write-mimo tests/analysis_capture_mimo.sc16
	./m2sdr_analyzer --report --json --mimo --file tests/analysis_capture_mimo.sc16 --nchannels 2 \
		--sample-rate 7.68e6 > tests/analysis_capture_mimo.json
	./tests/test_m2sdr_analysis_dual --check-mimo tests/analysis_capture_mimo.json

.PHONY: clean-analyzer-mimo
clean: clean-analyzer-mimo
clean-analyzer-mimo:
	rm -f tests/analysis_capture_mimo.sc16 tests/analysis_capture_mimo.json
