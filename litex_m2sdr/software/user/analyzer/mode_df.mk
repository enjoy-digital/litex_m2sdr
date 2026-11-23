# Report test of the direction finding mode of m2sdr_analyzer, part of test-analyzer (see the Makefile).
.PHONY: test-analyzer-df
test-analyzer: test-analyzer-df
test-analyzer-df: m2sdr_analyzer tests/test_m2sdr_analysis_dual
	./tests/test_m2sdr_analysis_dual --write-df tests/analysis_capture_df.sc16
	./m2sdr_analyzer --report --json --df --df-cal 40 --file tests/analysis_capture_df.sc16 --nchannels 2 \
		--sample-rate 1e6 --freq 2.4e9 > tests/analysis_capture_df.json
	./tests/test_m2sdr_analysis_dual --check-df tests/analysis_capture_df.json
	./tests/test_m2sdr_analysis_dual --write-df-broadside tests/analysis_capture_df0.sc16
	rm -f tests/analysis_capture_df_cal.csv
	./m2sdr_analyzer --report --df-cal-store --df-cal-file tests/analysis_capture_df_cal.csv \
		--file tests/analysis_capture_df0.sc16 --nchannels 2 --sample-rate 1e6 --freq 2.4e9 > /dev/null
	./m2sdr_analyzer --report --json --df --df-cal-file tests/analysis_capture_df_cal.csv \
		--file tests/analysis_capture_df.sc16 --nchannels 2 --sample-rate 1e6 --freq 2.4e9 > tests/analysis_capture_df.json
	./tests/test_m2sdr_analysis_dual --check-df tests/analysis_capture_df.json

.PHONY: clean-analyzer-df
clean: clean-analyzer-df
clean-analyzer-df:
	rm -f tests/analysis_capture_df.sc16 tests/analysis_capture_df.json tests/analysis_capture_df0.sc16 tests/analysis_capture_df_cal.csv
