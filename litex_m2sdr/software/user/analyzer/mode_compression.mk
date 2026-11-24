# Report test of the Compression mode of m2sdr_analyzer, part of test-analyzer (see the Makefile): its
# stimulus (--lab-stimulus-out) through a synthetic device, read back by the report.
.PHONY: test-analyzer-compression
test-analyzer: test-analyzer-compression
test-analyzer-compression: m2sdr_analyzer tests/test_m2sdr_analysis_lab
	./m2sdr_analyzer --sample-rate 1e6 --lab-tx-att 0 --compression --lab-stimulus-out tests/lab_comp_stim.sc16
	./tests/test_m2sdr_analysis_lab --write-compression tests/lab_comp_stim.sc16 tests/lab_comp.sc16
	./m2sdr_analyzer --sample-rate 1e6 --lab-tx-att 0 --report --json --compression --file tests/lab_comp.sc16 > tests/lab_comp.json
	./tests/test_m2sdr_analysis_lab --check-compression tests/lab_comp.json

.PHONY: clean-analyzer-compression
clean: clean-analyzer-compression
clean-analyzer-compression:
	rm -f tests/lab_comp*
