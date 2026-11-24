# Report test of the Allan deviation mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-adev
test-analyzer: test-analyzer-adev
test-analyzer-adev: m2sdr_analyzer tests/test_m2sdr_analysis_timing
	./tests/test_m2sdr_analysis_timing --write-adev tests/analysis_capture_adev.sc16
	./m2sdr_analyzer --report --json --adev --adev-carrier 100.0123e6 --file tests/analysis_capture_adev.sc16 \
		--sample-rate 1e6 --freq 100e6 > tests/analysis_capture_adev.json
	./tests/test_m2sdr_analysis_timing --check-adev tests/analysis_capture_adev.json
