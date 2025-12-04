from ConfigLoader import ConfigLoader
import os 
from datetime import datetime

class ExperimentManager:
    """ Manages experiment setup including VCD and config handling."""

    def __init__(self, experiment_name = None, config_file = "alltests_config.yaml", vcd_dir="vcd_outputs"):
        timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")

        self.config_file = config_file
        self.vcd_dir = vcd_dir
        self._create_vcd_dir()

        self.config = ConfigLoader(config_file)
        # Create experiment folder
        if experiment_name:
            self.experiment_name = f"{experiment_name}"
        else:
            # Default: timestamp-based
            self.experiment_name = f"{timestamp}"
        self.experiment_dir = self._create_experiment_folder()
    
    def _create_vcd_dir(self):
        """Create VCD directory if it doesn't exist."""
        os.makedirs(self.vcd_dir, exist_ok=True)
        print(f"[VCD] VCD output directory: {os.path.abspath(self.vcd_dir)}")
    
    def get_vcd_path(self):
        if not self.experiment_dir:
            raise RuntimeError("Experiment folder not created. Call create_experiment_folder() first.")
        
        vcd_filename = f"{self.experiment_name}.vcd"
        return os.path.join(self.experiment_dir, vcd_filename)
    
    
    def _create_experiment_folder(self):
        """Create a timestamped experiment folder inside the VCD directory."""
        experiment_dir = os.path.join(self.vcd_dir, self.experiment_name)
        os.makedirs(experiment_dir, exist_ok=True)
        
        print(f"[EXP] Experiment folder created: {os.path.abspath(experiment_dir)}")
        return experiment_dir

    def save_config_report(self):
        """
        Save configuration report to experiment folder.
        
        Args:
            config: TestConfig object
            test_name: Name of the test
        """
        if not self.experiment_dir:
            raise RuntimeError("Experiment folder not created. Call create_experiment_folder() first.")
        
        report = self.config.generate_config_report()
        report_path = os.path.join(self.experiment_dir, "config_report.txt")
        
        with open(report_path, 'w') as f:
            f.write(report)
        
        print(f"[EXP] Config report saved: {report_path}")
        return report_path
    
    def save_config_yaml(self):
        """
        Save the YAML configuration file to experiment folder.
        
        Args:
            config: TestConfig object
        """
        if not self.experiment_dir:
            raise RuntimeError("Experiment folder not created. Call create_experiment_folder() first.")
        
        yaml_content = self.config.export_config()
        yaml_path = os.path.join(self.experiment_dir, "config.yaml")
        
        with open(yaml_path, 'w') as f:
            f.write(yaml_content)
        
        print(f"[EXP] Config YAML saved: {yaml_path}")
        return yaml_path

    def generate_report(self):
        self.save_config_report()
        self.save_config_yaml()
