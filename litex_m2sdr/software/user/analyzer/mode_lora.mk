# Report test of the LoRa mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-lora
test-analyzer: test-analyzer-lora
test-analyzer-lora: m2sdr_analyzer tests/test_m2sdr_analysis_wireless
	./tests/test_m2sdr_analysis_wireless --write-lora tests/analysis_capture_lora.sc16
	./m2sdr_analyzer --report --json --lora --file tests/analysis_capture_lora.sc16 --nchannels 1 \
		> tests/analysis_capture_lora.json
	./tests/test_m2sdr_analysis_wireless --check-lora tests/analysis_capture_lora.json

.PHONY: clean-analyzer-lora
clean: clean-analyzer-lora
clean-analyzer-lora:
	rm -f tests/analysis_capture_lora.*
