/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR RF Utility.
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <getopt.h>

#include "ad9361/platform.h"
#include "ad9361/ad9361.h"
#include "ad9361/ad9361_api.h"

#include "liblitepcie.h"
#include "libm2sdr.h"

/* Parameters */
/*------------*/

#define DEFAULT_REFCLK_FREQ    ((int64_t)38400000) /* Reference Clock */
#define DEFAULT_SAMPLERATE               30720000  /* RF Samplerate */
#define DEFAULT_TX_FREQ      ((int64_t)2400000000) /* TX (TX1/2) Center Freq in Hz */
#define DEFAULT_RX_FREQ      ((int64_t)2400000000) /* RX (RX1/2) Center Freq in Hz */
#define DEFAULT_TX_GAIN                       -20  /* TX Gain in dB -89 ->  0 dB */
#define DEFAULT_RX_GAIN                         0  /* RX Gain in dB   0 -> 76 dB */
#define DEFAULT_LOOPBACK                        0  /* Internal loopback */

#define TX_FREQ_MIN   47000000 /* Hz */
#define TX_FREQ_MAX 6000000000 /* Hz */
#define RX_FREQ_MIN   70000000 /* Hz */
#define RX_FREQ_MAX 6000000000 /* Hz */
#define TX_GAIN_MIN        -89 /* dB */
#define TX_GAIN_MAX          0 /* dB */
#define RX_GAIN_MIN          0 /* dB */
#define RX_GAIN_MAX         76 /* dB */

/* Variables */
/*-----------*/

static char litepcie_device[1024];
static int litepcie_device_num;
static int litepcie_execute_and_exit;

sig_atomic_t keep_running = 1;

void intHandler(int dummy) {
    keep_running = 0;
}

/* AD9361 */
/*--------*/

#define AD9361_GPIO_RESET_PIN 0

struct ad9361_rf_phy *ad9361_phy;

int spi_write_then_read(struct spi_device *spi,
                        const unsigned char *txbuf, unsigned n_tx,
                        unsigned char *rxbuf, unsigned n_rx)
{

    int fd;

    fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }

    if (n_tx == 2 && n_rx == 1) {
        /* read */
        rxbuf[0] = m2sdr_ad9361_spi_read(fd, txbuf[0] << 8 | txbuf[1]);
    } else if (n_tx == 3 && n_rx == 0) {
        /* write */
        m2sdr_ad9361_spi_write(fd, txbuf[0] << 8 | txbuf[1], txbuf[2]);
    } else {
        fprintf(stderr, "Unsupported SPI transfer n_tx=%d n_rx=%d\n",
                n_tx, n_rx);
        exit(1);
    }

    close(fd);

    return 0;
}

void udelay(unsigned long usecs)
{
    usleep(usecs);
}

void mdelay(unsigned long msecs)
{
    usleep(msecs * 1000);
}

unsigned long msleep_interruptible(unsigned int msecs)
{
    usleep(msecs * 1000);
    return 0;
}

bool gpio_is_valid(int number)
{
 switch(number) {
    case AD9361_GPIO_RESET_PIN:
        return true;
    default:
        return false;
    }
}

void gpio_set_value(unsigned gpio, int value)
{

}

AD9361_InitParam default_init_param = {
    /* Device selection */
    ID_AD9361,  // dev_sel
    /* Identification number */
    0,      //id_no
    /* Reference Clock */
    DEFAULT_REFCLK_FREQ, //reference_clk_rate
    /* Base Configuration */
    1,      //two_rx_two_tx_mode_enable *** adi,2rx-2tx-mode-enable
    1,      //one_rx_one_tx_mode_use_rx_num *** adi,1rx-1tx-mode-use-rx-num
    1,      //one_rx_one_tx_mode_use_tx_num *** adi,1rx-1tx-mode-use-tx-num
    1,      //frequency_division_duplex_mode_enable *** adi,frequency-division-duplex-mode-enable
    0,      //frequency_division_duplex_independent_mode_enable *** adi,frequency-division-duplex-independent-mode-enable
    0,      //tdd_use_dual_synth_mode_enable *** adi,tdd-use-dual-synth-mode-enable
    0,      //tdd_skip_vco_cal_enable *** adi,tdd-skip-vco-cal-enable
    0,      //tx_fastlock_delay_ns *** adi,tx-fastlock-delay-ns
    0,      //rx_fastlock_delay_ns *** adi,rx-fastlock-delay-ns
    0,      //rx_fastlock_pincontrol_enable *** adi,rx-fastlock-pincontrol-enable
    0,      //tx_fastlock_pincontrol_enable *** adi,tx-fastlock-pincontrol-enable
    0,      //external_rx_lo_enable *** adi,external-rx-lo-enable
    0,      //external_tx_lo_enable *** adi,external-tx-lo-enable
    5,      //dc_offset_tracking_update_event_mask *** adi,dc-offset-tracking-update-event-mask
    6,      //dc_offset_attenuation_high_range *** adi,dc-offset-attenuation-high-range
    5,      //dc_offset_attenuation_low_range *** adi,dc-offset-attenuation-low-range
    0x28,   //dc_offset_count_high_range *** adi,dc-offset-count-high-range
    0x32,   //dc_offset_count_low_range *** adi,dc-offset-count-low-range
    0,      //split_gain_table_mode_enable *** adi,split-gain-table-mode-enable
    MAX_SYNTH_FREF, //trx_synthesizer_target_fref_overwrite_hz *** adi,trx-synthesizer-target-fref-overwrite-hz
    0,      // qec_tracking_slow_mode_enable *** adi,qec-tracking-slow-mode-enable
    /* ENSM Control */
    0,      //ensm_enable_pin_pulse_mode_enable *** adi,ensm-enable-pin-pulse-mode-enable
    0,      //ensm_enable_txnrx_control_enable *** adi,ensm-enable-txnrx-control-enable
    /* LO Control */
    DEFAULT_RX_FREQ, //rx_synthesizer_frequency_hz *** adi,rx-synthesizer-frequency-hz
    DEFAULT_TX_FREQ, //tx_synthesizer_frequency_hz *** adi,tx-synthesizer-frequency-hz
    1,              //tx_lo_powerdown_managed_enable *** adi,tx-lo-powerdown-managed-enable
    /* Rate & BW Control */
    {983040000, 245760000, 122880000, 61440000, 30720000, 30720000},// rx_path_clock_frequencies[6] *** adi,rx-path-clock-frequencies
    {983040000, 122880000, 122880000, 61440000, 30720000, 30720000},// tx_path_clock_frequencies[6] *** adi,tx-path-clock-frequencies
    18000000,//rf_rx_bandwidth_hz *** adi,rf-rx-bandwidth-hz
    18000000,//rf_tx_bandwidth_hz *** adi,rf-tx-bandwidth-hz
    /* RF Port Control */
    0,      //rx_rf_port_input_select *** adi,rx-rf-port-input-select
    0,      //tx_rf_port_input_select *** adi,tx-rf-port-input-select
    /* TX Attenuation Control */
    10000,  //tx_attenuation_mdB *** adi,tx-attenuation-mdB
    0,      //update_tx_gain_in_alert_enable *** adi,update-tx-gain-in-alert-enable
    /* Reference Clock Control */
    0,      //xo_disable_use_ext_refclk_enable *** adi,xo-disable-use-ext-refclk-enable
    {8, 5920},  //dcxo_coarse_and_fine_tune[2] *** adi,dcxo-coarse-and-fine-tune
    CLKOUT_DISABLE, //clk_output_mode_select *** adi,clk-output-mode-select
    /* Gain Control */
    2,      //gc_rx1_mode *** adi,gc-rx1-mode
    2,      //gc_rx2_mode *** adi,gc-rx2-mode
    58,     //gc_adc_large_overload_thresh *** adi,gc-adc-large-overload-thresh
    4,      //gc_adc_ovr_sample_size *** adi,gc-adc-ovr-sample-size
    47,     //gc_adc_small_overload_thresh *** adi,gc-adc-small-overload-thresh
    8192,   //gc_dec_pow_measurement_duration *** adi,gc-dec-pow-measurement-duration
    0,      //gc_dig_gain_enable *** adi,gc-dig-gain-enable
    800,    //gc_lmt_overload_high_thresh *** adi,gc-lmt-overload-high-thresh
    704,    //gc_lmt_overload_low_thresh *** adi,gc-lmt-overload-low-thresh
    24,     //gc_low_power_thresh *** adi,gc-low-power-thresh
    15,     //gc_max_dig_gain *** adi,gc-max-dig-gain
    /* Gain MGC Control */
    2,      //mgc_dec_gain_step *** adi,mgc-dec-gain-step
    2,      //mgc_inc_gain_step *** adi,mgc-inc-gain-step
    0,      //mgc_rx1_ctrl_inp_enable *** adi,mgc-rx1-ctrl-inp-enable
    0,      //mgc_rx2_ctrl_inp_enable *** adi,mgc-rx2-ctrl-inp-enable
    0,      //mgc_split_table_ctrl_inp_gain_mode *** adi,mgc-split-table-ctrl-inp-gain-mode
    /* Gain AGC Control */
    10,     //agc_adc_large_overload_exceed_counter *** adi,agc-adc-large-overload-exceed-counter
    2,      //agc_adc_large_overload_inc_steps *** adi,agc-adc-large-overload-inc-steps
    0,      //agc_adc_lmt_small_overload_prevent_gain_inc_enable *** adi,agc-adc-lmt-small-overload-prevent-gain-inc-enable
    10,     //agc_adc_small_overload_exceed_counter *** adi,agc-adc-small-overload-exceed-counter
    4,      //agc_dig_gain_step_size *** adi,agc-dig-gain-step-size
    3,      //agc_dig_saturation_exceed_counter *** adi,agc-dig-saturation-exceed-counter
    1000,   // agc_gain_update_interval_us *** adi,agc-gain-update-interval-us
    0,      //agc_immed_gain_change_if_large_adc_overload_enable *** adi,agc-immed-gain-change-if-large-adc-overload-enable
    0,      //agc_immed_gain_change_if_large_lmt_overload_enable *** adi,agc-immed-gain-change-if-large-lmt-overload-enable
    10,     //agc_inner_thresh_high *** adi,agc-inner-thresh-high
    1,      //agc_inner_thresh_high_dec_steps *** adi,agc-inner-thresh-high-dec-steps
    12,     //agc_inner_thresh_low *** adi,agc-inner-thresh-low
    1,      //agc_inner_thresh_low_inc_steps *** adi,agc-inner-thresh-low-inc-steps
    10,     //agc_lmt_overload_large_exceed_counter *** adi,agc-lmt-overload-large-exceed-counter
    2,      //agc_lmt_overload_large_inc_steps *** adi,agc-lmt-overload-large-inc-steps
    10,     //agc_lmt_overload_small_exceed_counter *** adi,agc-lmt-overload-small-exceed-counter
    5,      //agc_outer_thresh_high *** adi,agc-outer-thresh-high
    2,      //agc_outer_thresh_high_dec_steps *** adi,agc-outer-thresh-high-dec-steps
    18,     //agc_outer_thresh_low *** adi,agc-outer-thresh-low
    2,      //agc_outer_thresh_low_inc_steps *** adi,agc-outer-thresh-low-inc-steps
    1,      //agc_attack_delay_extra_margin_us; *** adi,agc-attack-delay-extra-margin-us
    0,      //agc_sync_for_gain_counter_enable *** adi,agc-sync-for-gain-counter-enable
    /* Fast AGC */
    64,     //fagc_dec_pow_measuremnt_duration ***  adi,fagc-dec-pow-measurement-duration
    260,    //fagc_state_wait_time_ns ***  adi,fagc-state-wait-time-ns
    /* Fast AGC - Low Power */
    0,      //fagc_allow_agc_gain_increase ***  adi,fagc-allow-agc-gain-increase-enable
    5,      //fagc_lp_thresh_increment_time ***  adi,fagc-lp-thresh-increment-time
    1,      //fagc_lp_thresh_increment_steps ***  adi,fagc-lp-thresh-increment-steps
    /* Fast AGC - Lock Level (Lock Level is set via slow AGC inner high threshold) */
    1,      //fagc_lock_level_lmt_gain_increase_en ***  adi,fagc-lock-level-lmt-gain-increase-enable
    5,      //fagc_lock_level_gain_increase_upper_limit ***  adi,fagc-lock-level-gain-increase-upper-limit
    /* Fast AGC - Peak Detectors and Final Settling */
    1,      //fagc_lpf_final_settling_steps ***  adi,fagc-lpf-final-settling-steps
    1,      //fagc_lmt_final_settling_steps ***  adi,fagc-lmt-final-settling-steps
    3,      //fagc_final_overrange_count ***  adi,fagc-final-overrange-count
    /* Fast AGC - Final Power Test */
    0,      //fagc_gain_increase_after_gain_lock_en ***  adi,fagc-gain-increase-after-gain-lock-enable
    /* Fast AGC - Unlocking the Gain */
    0,      //fagc_gain_index_type_after_exit_rx_mode ***  adi,fagc-gain-index-type-after-exit-rx-mode
    1,      //fagc_use_last_lock_level_for_set_gain_en ***  adi,fagc-use-last-lock-level-for-set-gain-enable
    1,      //fagc_rst_gla_stronger_sig_thresh_exceeded_en ***  adi,fagc-rst-gla-stronger-sig-thresh-exceeded-enable
    5,      //fagc_optimized_gain_offset ***  adi,fagc-optimized-gain-offset
    10,     //fagc_rst_gla_stronger_sig_thresh_above_ll ***  adi,fagc-rst-gla-stronger-sig-thresh-above-ll
    1,      //fagc_rst_gla_engergy_lost_sig_thresh_exceeded_en ***  adi,fagc-rst-gla-engergy-lost-sig-thresh-exceeded-enable
    1,      //fagc_rst_gla_engergy_lost_goto_optim_gain_en ***  adi,fagc-rst-gla-engergy-lost-goto-optim-gain-enable
    10,     //fagc_rst_gla_engergy_lost_sig_thresh_below_ll ***  adi,fagc-rst-gla-engergy-lost-sig-thresh-below-ll
    8,      //fagc_energy_lost_stronger_sig_gain_lock_exit_cnt ***  adi,fagc-energy-lost-stronger-sig-gain-lock-exit-cnt
    1,      //fagc_rst_gla_large_adc_overload_en ***  adi,fagc-rst-gla-large-adc-overload-enable
    1,      //fagc_rst_gla_large_lmt_overload_en ***  adi,fagc-rst-gla-large-lmt-overload-enable
    0,      //fagc_rst_gla_en_agc_pulled_high_en ***  adi,fagc-rst-gla-en-agc-pulled-high-enable
    0,      //fagc_rst_gla_if_en_agc_pulled_high_mode ***  adi,fagc-rst-gla-if-en-agc-pulled-high-mode
    64,     //fagc_power_measurement_duration_in_state5 ***  adi,fagc-power-measurement-duration-in-state5
    /* RSSI Control */
    1,      //rssi_delay *** adi,rssi-delay
    1000,   //rssi_duration *** adi,rssi-duration
    3,      //rssi_restart_mode *** adi,rssi-restart-mode
    0,      //rssi_unit_is_rx_samples_enable *** adi,rssi-unit-is-rx-samples-enable
    1,      //rssi_wait *** adi,rssi-wait
    /* Aux ADC Control */
    256,    //aux_adc_decimation *** adi,aux-adc-decimation
    40000000UL, //aux_adc_rate *** adi,aux-adc-rate
    /* AuxDAC Control */
    1,      //aux_dac_manual_mode_enable ***  adi,aux-dac-manual-mode-enable
    0,      //aux_dac1_default_value_mV ***  adi,aux-dac1-default-value-mV
    0,      //aux_dac1_active_in_rx_enable ***  adi,aux-dac1-active-in-rx-enable
    0,      //aux_dac1_active_in_tx_enable ***  adi,aux-dac1-active-in-tx-enable
    0,      //aux_dac1_active_in_alert_enable ***  adi,aux-dac1-active-in-alert-enable
    0,      //aux_dac1_rx_delay_us ***  adi,aux-dac1-rx-delay-us
    0,      //aux_dac1_tx_delay_us ***  adi,aux-dac1-tx-delay-us
    0,      //aux_dac2_default_value_mV ***  adi,aux-dac2-default-value-mV
    0,      //aux_dac2_active_in_rx_enable ***  adi,aux-dac2-active-in-rx-enable
    0,      //aux_dac2_active_in_tx_enable ***  adi,aux-dac2-active-in-tx-enable
    0,      //aux_dac2_active_in_alert_enable ***  adi,aux-dac2-active-in-alert-enable
    0,      //aux_dac2_rx_delay_us ***  adi,aux-dac2-rx-delay-us
    0,      //aux_dac2_tx_delay_us ***  adi,aux-dac2-tx-delay-us
    /* Temperature Sensor Control */
    256,    //temp_sense_decimation *** adi,temp-sense-decimation
    1000,   //temp_sense_measurement_interval_ms *** adi,temp-sense-measurement-interval-ms
    0xCE,   //temp_sense_offset_signed *** adi,temp-sense-offset-signed
    1,      //temp_sense_periodic_measurement_enable *** adi,temp-sense-periodic-measurement-enable
    /* Control Out Setup */
    0xFF,   //ctrl_outs_enable_mask *** adi,ctrl-outs-enable-mask
    0,      //ctrl_outs_index *** adi,ctrl-outs-index
    /* External LNA Control */
    0,      //elna_settling_delay_ns *** adi,elna-settling-delay-ns
    0,      //elna_gain_mdB *** adi,elna-gain-mdB
    0,      //elna_bypass_loss_mdB *** adi,elna-bypass-loss-mdB
    0,      //elna_rx1_gpo0_control_enable *** adi,elna-rx1-gpo0-control-enable
    0,      //elna_rx2_gpo1_control_enable *** adi,elna-rx2-gpo1-control-enable
    0,      //elna_gaintable_all_index_enable *** adi,elna-gaintable-all-index-enable
    /* Digital Interface Control */
    0,      //digital_interface_tune_skip_mode *** adi,digital-interface-tune-skip-mode
    0,      //digital_interface_tune_fir_disable *** adi,digital-interface-tune-fir-disable
    1,      //pp_tx_swap_enable *** adi,pp-tx-swap-enable
    1,      //pp_rx_swap_enable *** adi,pp-rx-swap-enable
    0,      //tx_channel_swap_enable *** adi,tx-channel-swap-enable
    0,      //rx_channel_swap_enable *** adi,rx-channel-swap-enable
    1,      //rx_frame_pulse_mode_enable *** adi,rx-frame-pulse-mode-enable
    0,      //two_t_two_r_timing_enable *** adi,2t2r-timing-enable
    0,      //invert_data_bus_enable *** adi,invert-data-bus-enable
    0,      //invert_data_clk_enable *** adi,invert-data-clk-enable
    0,      //fdd_alt_word_order_enable *** adi,fdd-alt-word-order-enable
    0,      //invert_rx_frame_enable *** adi,invert-rx-frame-enable
    0,      //fdd_rx_rate_2tx_enable *** adi,fdd-rx-rate-2tx-enable
    0,      //swap_ports_enable *** adi,swap-ports-enable
    0,      //single_data_rate_enable *** adi,single-data-rate-enable
    1,      //lvds_mode_enable *** adi,lvds-mode-enable
    0,      //half_duplex_mode_enable *** adi,half-duplex-mode-enable
    0,      //single_port_mode_enable *** adi,single-port-mode-enable
    0,      //full_port_enable *** adi,full-port-enable
    0,      //full_duplex_swap_bits_enable *** adi,full-duplex-swap-bits-enable
    0,      //delay_rx_data *** adi,delay-rx-data
    0,      //rx_data_clock_delay *** adi,rx-data-clock-delay
    4,      //rx_data_delay *** adi,rx-data-delay
    7,      //tx_fb_clock_delay *** adi,tx-fb-clock-delay
    0,      //tx_data_delay *** adi,tx-data-delay
    150,    //lvds_bias_mV *** adi,lvds-bias-mV
    1,      //lvds_rx_onchip_termination_enable *** adi,lvds-rx-onchip-termination-enable
    0,      //rx1rx2_phase_inversion_en *** adi,rx1-rx2-phase-inversion-enable
    0xFF,   //lvds_invert1_control *** adi,lvds-invert1-control
    0x0F,   //lvds_invert2_control *** adi,lvds-invert2-control
    /* GPO Control */
    0,      //gpo0_inactive_state_high_enable *** adi,gpo0-inactive-state-high-enable
    0,      //gpo1_inactive_state_high_enable *** adi,gpo1-inactive-state-high-enable
    0,      //gpo2_inactive_state_high_enable *** adi,gpo2-inactive-state-high-enable
    0,      //gpo3_inactive_state_high_enable *** adi,gpo3-inactive-state-high-enable
    0,      //gpo0_slave_rx_enable *** adi,gpo0-slave-rx-enable
    0,      //gpo0_slave_tx_enable *** adi,gpo0-slave-tx-enable
    0,      //gpo1_slave_rx_enable *** adi,gpo1-slave-rx-enable
    0,      //gpo1_slave_tx_enable *** adi,gpo1-slave-tx-enable
    0,      //gpo2_slave_rx_enable *** adi,gpo2-slave-rx-enable
    0,      //gpo2_slave_tx_enable *** adi,gpo2-slave-tx-enable
    0,      //gpo3_slave_rx_enable *** adi,gpo3-slave-rx-enable
    0,      //gpo3_slave_tx_enable *** adi,gpo3-slave-tx-enable
    0,      //gpo0_rx_delay_us *** adi,gpo0-rx-delay-us
    0,      //gpo0_tx_delay_us *** adi,gpo0-tx-delay-us
    0,      //gpo1_rx_delay_us *** adi,gpo1-rx-delay-us
    0,      //gpo1_tx_delay_us *** adi,gpo1-tx-delay-us
    0,      //gpo2_rx_delay_us *** adi,gpo2-rx-delay-us
    0,      //gpo2_tx_delay_us *** adi,gpo2-tx-delay-us
    0,      //gpo3_rx_delay_us *** adi,gpo3-rx-delay-us
    0,      //gpo3_tx_delay_us *** adi,gpo3-tx-delay-us
    /* Tx Monitor Control */
    37000,  //low_high_gain_threshold_mdB *** adi,txmon-low-high-thresh
    0,      //low_gain_dB *** adi,txmon-low-gain
    24,     //high_gain_dB *** adi,txmon-high-gain
    0,      //tx_mon_track_en *** adi,txmon-dc-tracking-enable
    0,      //one_shot_mode_en *** adi,txmon-one-shot-mode-enable
    511,    //tx_mon_delay *** adi,txmon-delay
    8192,   //tx_mon_duration *** adi,txmon-duration
    2,      //tx1_mon_front_end_gain *** adi,txmon-1-front-end-gain
    2,      //tx2_mon_front_end_gain *** adi,txmon-2-front-end-gain
    48,     //tx1_mon_lo_cm *** adi,txmon-1-lo-cm
    48,     //tx2_mon_lo_cm *** adi,txmon-2-lo-cm
    /* GPIO definitions */
    -1,     //gpio_resetb *** reset-gpios
    /* MCS Sync */
    -1,     //gpio_sync *** sync-gpios
    -1,     //gpio_cal_sw1 *** cal-sw1-gpios
    -1,     //gpio_cal_sw2 *** cal-sw2-gpios
    /* External LO clocks */
    NULL,   //(*ad9361_rfpll_ext_recalc_rate)()
    NULL,   //(*ad9361_rfpll_ext_round_rate)()
    NULL    //(*ad9361_rfpll_ext_set_rate)()
};

AD9361_RXFIRConfig rx_fir_config = {    // BPF PASSBAND 3/20 fs to 1/4 fs
    3, // rx
    0, // rx_gain
    1, // rx_dec
    {-4, -6, -37, 35, 186, 86, -284, -315,
     107, 219, -4, 271, 558, -307, -1182, -356,
     658, 157, 207, 1648, 790, -2525, -2553, 748,
     865, -476, 3737, 6560, -3583, -14731, -5278, 14819,
     14819, -5278, -14731, -3583, 6560, 3737, -476, 865,
     748, -2553, -2525, 790, 1648, 207, 157, 658,
     -356, -1182, -307, 558, 271, -4, 219, 107,
     -315, -284, 86, 186, 35, -37, -6, -4,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0}, // rx_coef[128]
     64, // rx_coef_size
     {0, 0, 0, 0, 0, 0}, //rx_path_clks[6]
     0 // rx_bandwidth
};

AD9361_TXFIRConfig tx_fir_config = {    // BPF PASSBAND 3/20 fs to 1/4 fs
    3, // tx
    -6, // tx_gain
    1, // tx_int
    {-4, -6, -37, 35, 186, 86, -284, -315,
     107, 219, -4, 271, 558, -307, -1182, -356,
     658, 157, 207, 1648, 790, -2525, -2553, 748,
     865, -476, 3737, 6560, -3583, -14731, -5278, 14819,
     14819, -5278, -14731, -3583, 6560, 3737, -476, 865,
     748, -2553, -2525, 790, 1648, 207, 157, 658,
     -356, -1182, -307, 558, 271, -4, 219, 107,
     -315, -284, 86, 186, 35, -37, -6, -4,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0}, // tx_coef[128]
     64, // tx_coef_size
     {0, 0, 0, 0, 0, 0}, // tx_path_clks[6]
     0 // tx_bandwidth
};

/* M2SDR Init */
/*------------*/

//#define BIST_TONE
//#define BIST_PRBS

static void m2sdr_init(
    uint32_t samplerate,
    int64_t  refclk_freq,
    int64_t  tx_freq,
    int64_t  rx_freq,
    int64_t  tx_gain,
    int64_t  rx_gain,
    uint8_t  loopback,
    uint8_t  execute_and_exit
) {
    int fd;

    fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }

    default_init_param.gpio_resetb  = AD9361_GPIO_RESET_PIN;
    default_init_param.gpio_sync    = -1;
    default_init_param.gpio_cal_sw1 = -1;
    default_init_param.gpio_cal_sw2 = -1;

    /* Initialize AD9361 SPI */
    m2sdr_ad9361_spi_init(fd);

    /* Initialize AD9361 RFIC */
    ad9361_init(&ad9361_phy, &default_init_param);

    /* Configure AD9361 TX/RX FIRs */
    ad9361_set_tx_fir_config(ad9361_phy, tx_fir_config);
    ad9361_set_rx_fir_config(ad9361_phy, rx_fir_config);

    /* Configure AD9361 Samplerate */
    ad9361_set_trx_clock_chain_freq(ad9361_phy, samplerate);

    /* Configure AD9361 TX Attenuation */
    ad9361_set_tx_atten(ad9361_phy, -tx_gain*1000, 1, 1, 1);

    /* Configure AD9361 RX Gain */
    ad9361_set_rx_rf_gain(ad9361_phy, 0, rx_gain);
    ad9361_set_rx_rf_gain(ad9361_phy, 1, rx_gain);

    /* Configure AD9361 RX->TX Loopback */
    ad9361_bist_loopback(ad9361_phy, loopback);

    /* Debug/Tests */
    printf("SPI Register 0x010—Parallel Port Configuration 1: %08x\n", m2sdr_ad9361_spi_read(fd, 0x10));
    printf("SPI Register 0x011—Parallel Port Configuration 2: %08x\n", m2sdr_ad9361_spi_read(fd, 0x11));
    printf("SPI Register 0x012—Parallel Port Configuration 3: %08x\n", m2sdr_ad9361_spi_read(fd, 0x12));

    printf("AD9361 Control: %08x\n", litepcie_readl(fd, CSR_AD9361_CONFIG_ADDR));

    litepcie_writel(fd, CSR_AD9361_PRBS_TX_ADDR, 0 * (1 << CSR_AD9361_PRBS_TX_ENABLE_OFFSET));

#ifdef BIST_TONE
    ad9361_bist_tone(ad9361_phy, BIST_INJ_RX, 1000000, 0, 0x0); /* 1MHz tone / 0dB / RX1&2 */
#endif

#ifdef BIST_PRBS
    int i;
    ad9361_bist_loopback(ad9361_phy, 1);
    litepcie_writel(fd, CSR_AD9361_PRBS_TX_ADDR, 1 * (1 << CSR_AD9361_PRBS_TX_ENABLE_OFFSET));
    for (i=0; i<10; i++) {
        int synced;
        synced = litepcie_readl(fd, CSR_AD9361_PRBS_RX_ADDR) & 0x1;
        printf("PRBS RX Synced: %d\n", synced);
        mdelay(1000);
    }
#endif
    close(fd);
}

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR RF init/config utility\n"
           "usage: m2sdr_rf [options] cmd [args...]\n"
           "\n"
           "Options:\n"
           "-h                    Help.\n"
           "-c device_num         Select the device (default=0).\n"
           "-x                    Execute and exit.\n"
           "\n"
           "-refclk_freq freq     Set the RefClk frequency in Hz (default=%" PRId64 ").\n"
           "-samplerate sps       Set RF Samplerate in SPS (default=%d).\n"
           "-tx_freq freq         Set the TX (TX1/2) frequency in Hz (default=%" PRId64 ").\n"
           "-rx_freq freq         Set the RX (RX1/2) frequency in Hz (default=%" PRId64 ").\n"
           "-tx_gain gain         Set the TX gain in dB (default=%d).\n"
           "-rx_gain gain         Set the RX gain in dB (default=%d).\n"
           "-loopback enable      Set the internal loopback (JESD Deframer -> Framer) (default=%d).\n"
           "\n",
           DEFAULT_REFCLK_FREQ,
           DEFAULT_SAMPLERATE,
           DEFAULT_TX_FREQ,
           DEFAULT_RX_FREQ,
           DEFAULT_TX_GAIN,
           DEFAULT_RX_GAIN,
           DEFAULT_LOOPBACK);
    exit(1);
}

static struct option options[] = {
    { "help",             no_argument, NULL, 'h' },   /*  0 */
    { "execute_and_exit", no_argument, NULL, 'x' },   /*  1 */
    { "refclk_freq",      required_argument },        /*  2 */
    { "samplerate",       required_argument },        /*  3 */
    { "tx_freq",          required_argument },        /*  4 */
    { "rx_freq",          required_argument },        /*  5 */
    { "tx_gain",          required_argument },        /*  6 */
    { "rx_gain",          required_argument },        /*  7 */
    { "loopback",         required_argument },        /*  8 */
    { NULL },
};

/* Main */
/*------*/

int main(int argc, char **argv)
{
    int c;
    int option_index;

    litepcie_device_num       = 0;
    litepcie_execute_and_exit = 0;

    int64_t  refclk_freq;
    uint32_t samplerate;
    int64_t  tx_freq, rx_freq;
    int64_t  tx_gain, rx_gain;
    uint8_t  loopback;

    refclk_freq  = DEFAULT_REFCLK_FREQ;
    samplerate   = DEFAULT_SAMPLERATE;
    tx_freq      = DEFAULT_TX_FREQ;
    rx_freq      = DEFAULT_RX_FREQ;
    tx_gain      = DEFAULT_TX_GAIN;
    rx_gain      = DEFAULT_RX_GAIN;
    loopback     = DEFAULT_LOOPBACK;

    /* Parse/Handle Parameters. */
    for (;;) {
        c = getopt_long_only(argc, argv, "hx", options, &option_index);
        if (c == -1)
            break;
        switch(c) {
        case 0 :
            switch(option_index) {
                case 2: /* refclk_freq */
                    refclk_freq = (int64_t)strtod(optarg, NULL);
                    break;
                case 3: /* samplerate */
                    samplerate = (uint32_t)strtod(optarg, NULL);
                    break;
                case 4: /* tx_freq */
                    tx_freq = (int64_t)strtod(optarg, NULL);
                    break;
                case 5: /* rx_freq */
                    rx_freq = (int64_t)strtod(optarg, NULL);
                    break;
                case 6: /* tx_gain */
                    tx_gain = (int64_t)strtod(optarg, NULL);
                    break;
                case 7: /* rx_gain */
                    rx_gain = (int64_t)strtod(optarg, NULL);
                    break;
                case 8: /* loopback */
                    loopback = (uint8_t)strtod(optarg, NULL);
                    break;
                default:
                    fprintf(stderr, "unknown option index: %d\n", option_index);
                    exit(1);
            }
            break;
        case 'h':
            help();
            exit(1);
            break;
        case 'c':
            litepcie_device_num = atoi(optarg);
            break;
        case 'x':
            litepcie_execute_and_exit = 1;
            break;
        default:
            exit(1);
        }
    }

    /* Select device. */
    snprintf(litepcie_device, sizeof(litepcie_device), "/dev/litepcie%d", litepcie_device_num);

    /* Initialize RF. */
    m2sdr_init(samplerate, refclk_freq, tx_freq, rx_freq, tx_gain, rx_gain, loopback, litepcie_execute_and_exit);

    return 0;
}
