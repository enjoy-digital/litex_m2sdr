#!/usr/bin/env python3

import subprocess
import time

def initialize_ad9361():
    success_message = "AD936x Rev 2 successfully initialized"
    error_messages = [
        "Calibration TIMEOUT",
        "Floating point exception",
        "AD936x initialization error"
    ]

    attempt_count = 0

    while True:
        try:
            attempt_count += 1
            result = subprocess.run(["./m2sdr_rf", "init"], capture_output=True, text=True)
            output = result.stdout + result.stderr

            # Print the output of the command
            print(output)

            # Check for error messages first
            if any(error_message in output for error_message in error_messages):
                print("Initialization failed, retrying...")
                time.sleep(1)  # wait for a second before retrying
                continue  # skip to the next iteration

            # Check for the success message
            if success_message in output:
                print("Initialization successful.")
                print(f"Number of attempts: {attempt_count}")
                return True

        except Exception as e:
            print(f"An error occurred: {e}")
            time.sleep(1)  # wait for a second before retrying

if __name__ == "__main__":
    initialize_ad9361()
