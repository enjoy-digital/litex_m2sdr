# Report test of the DAB mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-dab
test-analyzer: test-analyzer-dab
test-analyzer-dab: m2sdr_analyzer tests/test_m2sdr_analysis
	./tests/test_m2sdr_analysis --write-dab tests/analysis_capture_dab.sc16
	./m2sdr_analyzer --report --json --dab --file tests/analysis_capture_dab.sc16 \
		> tests/analysis_capture_dab.json
	./tests/test_m2sdr_analysis --check-dab tests/analysis_capture_dab.json
