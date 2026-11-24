# Report test of the Sweep mode of m2sdr_analyzer, part of test-analyzer (see the Makefile): its
# stimulus (--lab-stimulus-out) through a synthetic device, read back by the report.
.PHONY: test-analyzer-sweep
test-analyzer: test-analyzer-sweep
test-analyzer-sweep: m2sdr_analyzer tests/test_m2sdr_analysis_lab
	./m2sdr_analyzer --sample-rate 1e6 --lab-tx-att 0 --sweep --lab-stimulus-out tests/lab_sweep_stim.sc16
	./tests/test_m2sdr_analysis_lab --write-sweep tests/lab_sweep_stim.sc16 tests/lab_sweep_thru.sc16 tests/lab_sweep_dut.sc16
	rm -f tests/lab_sweep_thru.csv
	./m2sdr_analyzer --sample-rate 1e6 --lab-tx-att 0 --report --sweep --sweep-thru tests/lab_sweep_thru.csv --sweep-thru-save \
		--file tests/lab_sweep_thru.sc16 > /dev/null
	./m2sdr_analyzer --sample-rate 1e6 --lab-tx-att 0 --report --json --sweep --sweep-thru tests/lab_sweep_thru.csv \
		--file tests/lab_sweep_dut.sc16 > tests/lab_sweep.json
	./tests/test_m2sdr_analysis_lab --check-sweep tests/lab_sweep.json

.PHONY: clean-analyzer-sweep
clean: clean-analyzer-sweep
clean-analyzer-sweep:
	rm -f tests/lab_sweep*
