# Report test of the pulse mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-pulse
test-analyzer: test-analyzer-pulse
test-analyzer-pulse: m2sdr_analyzer tests/test_m2sdr_analysis_monitoring
	./tests/test_m2sdr_analysis_monitoring --write-pulse tests/analysis_capture_pulse.sc16
	./m2sdr_analyzer --report --json --pulses --file tests/analysis_capture_pulse.sc16 --nchannels 1 \
		--sample-rate 10e6 --freq 5.6e9 > tests/analysis_capture_pulse.json
	./tests/test_m2sdr_analysis_monitoring --check-pulse tests/analysis_capture_pulse.json

.PHONY: clean-analyzer-pulse
clean: clean-analyzer-pulse
clean-analyzer-pulse:
	rm -f tests/analysis_capture_pulse.sc16 tests/analysis_capture_pulse.json
