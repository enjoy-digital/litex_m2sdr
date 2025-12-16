# Run the Test Bench:
```
  >> ./scheduler_tb.py -h
  [VCD] VCD output directory: /home/sens/litex_m2sdr/sim/scheduler_sim_workflow/vcd_outputs
  [EXP] Experiment folder created: /home/sens/litex_m2sdr/sim/scheduler_sim_workflow/vcd_outputs/2025-12-16_11-48-56
  usage: scheduler_tb.py [-h] [--config-file CONFIG_FILE] [--gtk] [--vcd-dir VCD_DIR] [--xp-name XP_NAME]

  Scheduler Testbench - LiteX-M2SDR Simulation

  options:
    -h, --help            show this help message and exit
    --config-file CONFIG_FILE
                          YAML configuration file for tests
    --gtk                 Open GTKWave at end of simulation
    --vcd-dir VCD_DIR     Directory to store VCD files
    --xp-name XP_NAME     Name of the experiment (used in folder and VCD naming)

              Examples:
              # Run alltest_config.yaml (default) and save VCD
              python scheduler_tb.py
              
              # Run to run specific test that will be saved later as mytest.vcd
              python scheduler_tb.py --xp-name mytest
              
              # Open latest experiment VCD in GTKWave automatically
              python scheduler_tb.py --gtk
``` 
# Send the vcd via ssh for GTKwave visualization 
On the client open WSL and type:
```
scp sens@sensnuc6:~/litex_m2sdr/sim/scheduler_sim_workflow/vcd_outputs/$xp-name$/$xp-name$.vcd .
gtkwave $xp-name$.vcd
```