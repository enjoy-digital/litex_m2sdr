# Report test of the AM/AM mode of m2sdr_analyzer, part of test-analyzer (see the Makefile): its
# stimulus (--lab-stimulus-out) through a synthetic device, read back by the report.
.PHONY: test-analyzer-amam
test-analyzer: test-analyzer-amam
test-analyzer-amam: m2sdr_analyzer tests/test_m2sdr_analysis_lab
	./m2sdr_analyzer --sample-rate 1e6 --lab-tx-att 0 --amam --lab-stimulus-out tests/lab_amam_stim.sc16
	./tests/test_m2sdr_analysis_lab --write-amam tests/lab_amam_stim.sc16 tests/lab_amam.sc16
	./m2sdr_analyzer --sample-rate 1e6 --lab-tx-att 0 --report --json --amam --file tests/lab_amam.sc16 > tests/lab_amam.json
	./tests/test_m2sdr_analysis_lab --check-amam tests/lab_amam.json

.PHONY: clean-analyzer-amam
clean: clean-analyzer-amam
clean-analyzer-amam:
	rm -f tests/lab_amam*
