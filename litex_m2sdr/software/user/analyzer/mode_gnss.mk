# Report test of the GPS L1 mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-gnss
test-analyzer: test-analyzer-gnss
test-analyzer-gnss: m2sdr_analyzer tests/test_m2sdr_analysis_timing
	./tests/test_m2sdr_analysis_timing --write-gnss tests/analysis_capture_gnss.sc16
	./m2sdr_analyzer --report --json --gnss --file tests/analysis_capture_gnss.sc16 --freq 1575.42e6 \
		> tests/analysis_capture_gnss.json
	./tests/test_m2sdr_analysis_timing --check-gnss tests/analysis_capture_gnss.json
