# Report test of the NR mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-nr
test-analyzer: test-analyzer-nr
test-analyzer-nr: m2sdr_analyzer tests/test_m2sdr_analysis_cellular
	./tests/test_m2sdr_analysis_cellular --write-nr tests/analysis_capture_nr.sc16
	./m2sdr_analyzer --report --json --nr --freq 3.5e9 --file tests/analysis_capture_nr.sc16 \
		> tests/analysis_capture_nr.json
	./tests/test_m2sdr_analysis_cellular --check-nr tests/analysis_capture_nr.json
