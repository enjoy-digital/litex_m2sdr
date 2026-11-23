# Report test of the coherence mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-coherence
test-analyzer: test-analyzer-coherence
test-analyzer-coherence: m2sdr_analyzer tests/test_m2sdr_analysis_dual
	./tests/test_m2sdr_analysis_dual --write-coherence tests/analysis_capture_coh.sc16
	./m2sdr_analyzer --report --json --coherence --file tests/analysis_capture_coh.sc16 --nchannels 2 \
		--sample-rate 10e6 --freq 100e6 > tests/analysis_capture_coh.json
	./tests/test_m2sdr_analysis_dual --check-coherence tests/analysis_capture_coh.json

.PHONY: clean-analyzer-coherence
clean: clean-analyzer-coherence
clean-analyzer-coherence:
	rm -f tests/analysis_capture_coh.sc16 tests/analysis_capture_coh.json
