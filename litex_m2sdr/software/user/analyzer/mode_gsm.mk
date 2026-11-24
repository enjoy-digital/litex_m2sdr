# Report test of the GSM mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-gsm
test-analyzer: test-analyzer-gsm
test-analyzer-gsm: m2sdr_analyzer tests/test_m2sdr_analysis_cellular
	./tests/test_m2sdr_analysis_cellular --write-gsm tests/analysis_capture_gsm.sc16
	./m2sdr_analyzer --report --json --gsm --freq 947.4e6 --file tests/analysis_capture_gsm.sc16 \
		> tests/analysis_capture_gsm.json
	./tests/test_m2sdr_analysis_cellular --check-gsm tests/analysis_capture_gsm.json
