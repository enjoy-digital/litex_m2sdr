# Report test of the IMD mode of m2sdr_analyzer, part of test-analyzer (see the Makefile): its
# stimulus (--lab-stimulus-out) through a synthetic device, read back by the report.
.PHONY: test-analyzer-imd
test-analyzer: test-analyzer-imd
test-analyzer-imd: m2sdr_analyzer tests/test_m2sdr_analysis_lab
	./m2sdr_analyzer --sample-rate 1e6 --lab-tx-att 0 --imd --lab-stimulus-out tests/lab_imd_stim.sc16
	./tests/test_m2sdr_analysis_lab --write-imd tests/lab_imd_stim.sc16 tests/lab_imd.sc16
	./m2sdr_analyzer --sample-rate 1e6 --lab-tx-att 0 --report --json --imd --file tests/lab_imd.sc16 > tests/lab_imd.json
	./tests/test_m2sdr_analysis_lab --check-imd tests/lab_imd.json

.PHONY: clean-analyzer-imd
clean: clean-analyzer-imd
clean-analyzer-imd:
	rm -f tests/lab_imd*
