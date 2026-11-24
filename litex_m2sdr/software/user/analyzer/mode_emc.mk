# Report test of the EMC mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-emc
test-analyzer: test-analyzer-emc
test-analyzer-emc: m2sdr_analyzer tests/test_m2sdr_analysis_monitoring
	./tests/test_m2sdr_analysis_monitoring --write-emc tests/analysis_capture_emc.sc16 tests/analysis_capture_emc_limit.txt \
		tests/analysis_capture_emc_factors.txt
	./m2sdr_analyzer --report --json --emc --emc-band cd --file tests/analysis_capture_emc.sc16 --nchannels 1 \
		--sample-rate 1048576 --freq 100e6 --emc-limit-qp tests/analysis_capture_emc_limit.txt \
		--emc-factors tests/analysis_capture_emc_factors.txt > tests/analysis_capture_emc.json
	./tests/test_m2sdr_analysis_monitoring --check-emc tests/analysis_capture_emc.json

.PHONY: clean-analyzer-emc
clean: clean-analyzer-emc
clean-analyzer-emc:
	rm -f tests/analysis_capture_emc.sc16 tests/analysis_capture_emc.json tests/analysis_capture_emc_limit.txt \
		tests/analysis_capture_emc_factors.txt
