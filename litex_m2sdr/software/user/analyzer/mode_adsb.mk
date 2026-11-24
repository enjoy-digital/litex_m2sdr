# Report test of the ADS-B mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-adsb
test-analyzer: test-analyzer-adsb
test-analyzer-adsb: m2sdr_analyzer tests/test_m2sdr_analysis_wireless
	./tests/test_m2sdr_analysis_wireless --write-adsb tests/analysis_capture_adsb.sc16
	./m2sdr_analyzer --report --json --adsb --file tests/analysis_capture_adsb.sc16 --nchannels 1 \
		> tests/analysis_capture_adsb.json
	./tests/test_m2sdr_analysis_wireless --check-adsb tests/analysis_capture_adsb.json

.PHONY: clean-analyzer-adsb
clean: clean-analyzer-adsb
clean-analyzer-adsb:
	rm -f tests/analysis_capture_adsb.*
