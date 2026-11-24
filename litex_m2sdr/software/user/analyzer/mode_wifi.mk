# Report test of the Wi-Fi mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-wifi
test-analyzer: test-analyzer-wifi
test-analyzer-wifi: m2sdr_analyzer tests/test_m2sdr_analysis_wireless
	./tests/test_m2sdr_analysis_wireless --write-wifi tests/analysis_capture_wifi.sc16
	./m2sdr_analyzer --report --json --wifi --file tests/analysis_capture_wifi.sc16 --nchannels 1 \
		> tests/analysis_capture_wifi.json
	./tests/test_m2sdr_analysis_wireless --check-wifi tests/analysis_capture_wifi.json

.PHONY: clean-analyzer-wifi
clean: clean-analyzer-wifi
clean-analyzer-wifi:
	rm -f tests/analysis_capture_wifi.*
