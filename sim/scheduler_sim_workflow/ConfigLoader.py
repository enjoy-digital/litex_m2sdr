#!/usr/bin/env python3

import yaml
import os
from datetime import datetime

class ConfigLoader:
    """Load and manage test configuration from YAML."""
    
    def __init__(self, config_file="alltest_config.yaml"):
        """Load the YAML config file."""
        self.config_file = config_file
        self.config = self._load_yaml()
        self.global_params = self.config.get("global", {})
        self.tests = self.config.get("tests", {})
    
    def _load_yaml(self):
        """Load YAML file."""
        if not os.path.exists(self.config_file):
            raise FileNotFoundError(f"Config file not found: {self.config_file}")
        
        with open(self.config_file, 'r') as f:
            return yaml.safe_load(f)
    
    def get_global_param(self, key, default=None):
        """Get a global parameter."""
        return self.global_params.get(key, default)
    
    def get_test(self, test_name):
        """Get a specific test configuration."""
        if test_name not in self.tests:
            raise KeyError(f"Test '{test_name}' not found in config")
        return self.tests[test_name]
    
    def get_all_tests(self):
        """Get all test names."""
        return list(self.tests.keys())
    
    def list_tests(self, print_output=True):
        """Print all available tests."""
        output = "\n[Config] Available Tests:\n"
        for test_name, test_cfg in self.tests.items():
            desc = test_cfg.get("description", "No description")
            output += f"  - {test_name}: {desc}\n"
        
        if print_output:
            print(output, end='')
        return output
    
    def list_global_params(self, print_output=True):
        """Print all global parameters."""
        output = "\n[Config] Global Parameters:\n"
        for key, value in self.global_params.items():
            output += f"  - {key}: {value}\n"
        
        if print_output:
            print(output, end='')
        return output
    
    def export_config(self):
        """Export the configuration as YAML string."""
        return yaml.dump(self.config, default_flow_style=False, sort_keys=False)
    
    def generate_config_report(self):
        """Generate a comprehensive configuration report."""
        report = ""
        report += "=" * 70 + "\n"
        report += "SCHEDULER TESTBENCH CONFIGURATION REPORT\n"
        report += f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n"
        report += f"Config File: {os.path.abspath(self.config_file)}\n"
        report += "=" * 70 + "\n"
        report += self.list_global_params(print_output=False)
        report += self.list_tests(print_output=False)
        return report

        


# Example usage
if __name__ == "__main__":
    config = ConfigLoader("test_config.yaml")
    report = config.generate_config_report()
    print(report)