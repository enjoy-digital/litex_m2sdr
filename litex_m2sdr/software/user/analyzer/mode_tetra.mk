# Report test of the TETRA mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-tetra
test-analyzer: test-analyzer-tetra
test-analyzer-tetra: m2sdr_analyzer tests/test_m2sdr_analysis_cellular
	./tests/test_m2sdr_analysis_cellular --write-tetra tests/analysis_capture_tetra.sc16
	./m2sdr_analyzer --report --json --tetra --freq 390e6 --file tests/analysis_capture_tetra.sc16 \
		> tests/analysis_capture_tetra.json
	./tests/test_m2sdr_analysis_cellular --check-tetra tests/analysis_capture_tetra.json
