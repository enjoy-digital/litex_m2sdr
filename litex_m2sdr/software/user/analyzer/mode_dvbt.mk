# Report test of the DVB-T mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-dvbt
test-analyzer: test-analyzer-dvbt
test-analyzer-dvbt: m2sdr_analyzer tests/test_m2sdr_analysis
	./tests/test_m2sdr_analysis --write-dvbt tests/analysis_capture_dvbt.sc16
	./m2sdr_analyzer --report --json --dvbt --file tests/analysis_capture_dvbt.sc16 \
		> tests/analysis_capture_dvbt.json
	./tests/test_m2sdr_analysis --check-dvbt tests/analysis_capture_dvbt.json
