# Report test of the LTE mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-lte
test-analyzer: test-analyzer-lte
test-analyzer-lte: m2sdr_analyzer tests/test_m2sdr_analysis
	./tests/test_m2sdr_analysis --write-lte tests/analysis_capture_lte.sc16
	./m2sdr_analyzer --report --json --lte --file tests/analysis_capture_lte.sc16 --sample-rate 7.68e6 \
		> tests/analysis_capture_lte.json
	./tests/test_m2sdr_analysis --check-lte tests/analysis_capture_lte.json
