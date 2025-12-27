#!/usr/bin/env python3

import sys
import argparse
import csv

from litex import RemoteClient
from litescope import LiteScopeAnalyzerDriver


def list_analyzer_signals(csv_file="analyzer.csv"):
        csv_reader = csv.reader(open(csv_file), delimiter=',', quotechar='#')
        for idx,item in enumerate(csv_reader):
            t, _, n, _ = item
            if t == "signal":
                print(f"{idx}: {n}")

def get_trigger_signal(idx, csv_file="analyzer.csv"):
        assert idx is not None, "Trigger index must be specified."
        csv_reader = csv.reader(open(csv_file), delimiter=',', quotechar='#')
        for i,item in enumerate(csv_reader):
            t, _, n, _ = item
            if t == "signal" and i == idx:
                return str(n)
        raise ValueError(f"Signal with index {idx} not found in {csv_file}.")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--trig",  action="store_true", help="Trigger on Ready rising edge.")
    parser.add_argument("--list",     action="store_true", help="List analyzer signals.")
    parser.add_argument("--header", action="store_true", help="Trigger when header is seen.")
    parser.add_argument("--frame-count", action="store_true", help="Trigger when frame count reaches 1.")
    parser.add_argument("--offset",    default=128,         help="Capture Offset.")
    parser.add_argument("--length",    default=512,         help="Capture Length.")
    parser.add_argument("--filename",  default="dump",      help="Output filename.")
    args = parser.parse_args()

    wb = RemoteClient()
    wb.open()

    # # #
    analyzer = LiteScopeAnalyzerDriver(wb.regs, "analyzer", debug=True)
    analyzer.configure_group(0)
    if args.list:
        list_analyzer_signals()
        sys.exit(0)
    if args.trig:    
        list_analyzer_signals()
        while True:
            trig_idx = int(input("Choose the signal number to trigger on: "))
            trigger_signal = get_trigger_signal(trig_idx)
            input(f"Using trigger signal: {trigger_signal}\nPress to confirm")
            break
        while True:
            trig_pattern = input("\nChoose trigger pattern:\n\t1: rising Edge\n\t2: falling Edge\nEnter choice: ")
            if trig_pattern == "1":
                analyzer.add_rising_edge_trigger(trigger_signal)
                break
            elif trig_pattern == "2":
                analyzer.add_falling_edge_trigger(trigger_signal)
                break
            else:
                print("Invalid trigger pattern. Please enter 1 or 2.")
    elif args.header:
        list_analyzer_signals()
        value = input("Do you want to specify a different trigger signal than 'basesoc_basesoc_buffering_next_source_payload_data'? (y/n): ")
        if value.lower() == 'y':
            while True:
                trig_idx = int(input("Choose the signal number to trigger on: "))
                trigger_signal = get_trigger_signal(trig_idx)
                input(f"Using trigger signal: {trigger_signal}\nPress to confirm")
                break
        else:
            trigger_signal = "basesoc_basesoc_buffering_next_source_payload_data"
        value = input("Do you want to specify a different trigger header value than 0x5aa55aa55aa55aa5? (y/n): ")
        if value.lower() == 'y':
            header = int(input("Enter the trigger header value in hex (default is 0x5aa55aa55aa55aa5): "), 16)
            if not header:
                header = 0x5aa55aa55aa55aa5
        else:
            header = 0x5aa55aa55aa55aa5
        analyzer.configure_trigger(cond={str(trigger_signal): f"0b{header:064b}"})
    elif args.frame_count:
        trigger_signal = "basesoc_ad9361_scheduler_frame_count"
        analyzer.configure_trigger(cond={str(trigger_signal): f"0b{1:064b}"})
    
    analyzer.configure_subsampler(1)
    analyzer.run(offset=int(args.offset), length=int(args.length))
    analyzer.wait_done()
    analyzer.upload()
    if args.filename:
        analyzer.save("vcd/" + str(args.filename) + ".vcd")
    else:
        analyzer.save("vcd/" + trigger_signal + ".vcd")
    # # #

    wb.close()

if __name__ == "__main__":
    main()
