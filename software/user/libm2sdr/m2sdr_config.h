/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Default Config.
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdlib.h>
#include <stdio.h>

#include "../ad9361/platform.h"
#include "../ad9361/ad9361.h"
#include "../ad9361/ad9361_api.h"

/* Parameters */
/*------------*/

#define DEFAULT_REFCLK_FREQ    ((int64_t)38400000) /* Reference Clock */
#define DEFAULT_SAMPLERATE               30720000  /* RF Samplerate */
#define DEFAULT_BANDWIDTH                56000000  /* RF Bandwidth */
#define DEFAULT_TX_FREQ      ((int64_t) 2400000000) /* TX (TX1/2) Center Freq in Hz */
#define DEFAULT_RX_FREQ      ((int64_t) 2400000000) /* RX (RX1/2) Center Freq in Hz */
#define DEFAULT_TX_GAIN                       -20  /* TX Gain in dB -89 ->  0 dB */
#define DEFAULT_RX_GAIN                         0  /* RX Gain in dB   0 -> 76 dB */
#define DEFAULT_LOOPBACK                        0  /* Internal loopback */
#define DEFAULT_BIST_TONE_FREQ            1000000  /* BIST Tone Freq in Hz */

#define TX_FREQ_MIN   47000000 /* Hz */
#define TX_FREQ_MAX 6000000000 /* Hz */
#define RX_FREQ_MIN   70000000 /* Hz */
#define RX_FREQ_MAX 6000000000 /* Hz */
#define TX_GAIN_MIN        -89 /* dB */
#define TX_GAIN_MAX          0 /* dB */
#define RX_GAIN_MIN          0 /* dB */
#define RX_GAIN_MAX         76 /* dB */

#define TX_CLK_DELAY  0
#define TX_DAT_DELAY  5
#define RX_CLK_DELAY  2
#define RX_DAT_DELAY  3

#define SI5351_I2C_ADDR 0x60
#define SI5351B_VERSION 0b0
#define SI5351C_VERSION 0b1
#define SI5351C_10MHZ_CLK_IN_FROM_PLL 0b0
#define SI5351C_10MHZ_CLK_IN_FROM_UFL 0b1

/* SI5351B-C Default Config from XO (38.4MHz on all outputs) */
/*-----------------------------------------------------------*/

const uint8_t si5351_xo_config[][2] = {
    /* Interrupt Mask Configuration */
    { 0x02, 0x33 },  // Int masks: CLK_LOS(1), LOL_A(1) enabled, XO_LOS(0), LOL_B(0), SYS_INIT(0) disabled

    /* Output Enable Control */
    { 0x03, 0x00 },  // All CLK outputs enabled via register (OEB pin disabled)

    /* PLL Reset Control */
    { 0x04, 0x10 },  // Disable reset on PLLA LOS (bit4=1), PLLB reset normal (bit5=0)

    /* I2C Configuration */
    { 0x07, 0x01 },  // I2C address: 0x60 (default)

    /* Clock Input Configuration */
    { 0x0F, 0x00 },  // PLL src=XTAL (25MHz), CLKIN_DIV=1

    /* Output Channel Configuration (CLK0-CLK7) */
    { 0x10, 0x2F },  // CLK0: LVCMOS 8mA, MS0 src=PLLB
    { 0x11, 0x2F },  // CLK1: LVCMOS 8mA, MS1 src=PLLB
    { 0x12, 0x2F },  // CLK2: LVCMOS 8mA, MS2 src=PLLB
    { 0x13, 0x2F },  // CLK3: LVCMOS 8mA, MS3 src=PLLB
    { 0x14, 0x2F },  // CLK4: LVCMOS 8mA, MS4 src=PLLB
    { 0x15, 0x2F },  // CLK5: LVCMOS 8mA, MS5 src=PLLB
    { 0x16, 0x2F },  // CLK6: LVCMOS 8mA, MS6 src=PLLB
    { 0x17, 0x2F },  // CLK7: LVCMOS 8mA, MS7 src=PLLB

    /* PLLB Configuration (VCO = 844.8MHz from 25MHz XTAL) */
    { 0x22, 0x42 },  // PLLB feedback: Multisynth NA (integer mode)
    { 0x23, 0x40 },  // PLLB reset=normal
    { 0x24, 0x00 },
    { 0x25, 0x0E },
    { 0x26, 0xE5 },
    { 0x27, 0xF5 },
    { 0x28, 0xBC },
    { 0x29, 0xC0 },

    /* MS0 Configuration (Output Divider 22 for 38.4MHz) */
    { 0x2A, 0x00 },
    { 0x2B, 0x01 },
    { 0x2C, 0x00 },
    { 0x2D, 0x09 },
    { 0x2E, 0x00 },
    { 0x2F, 0x00 },
    { 0x30, 0x00 },
    { 0x31, 0x00 },

    /* MS1 Configuration (Output Divider 8.448 for 100MHz) */
    { 0x32, 0x00 },
    { 0x33, 0x7D },
    { 0x34, 0x00 },
    { 0x35, 0x02 },
    { 0x36, 0x39 },
    { 0x37, 0x00 },
    { 0x38, 0x00 },
    { 0x39, 0x2B },

    /* MS2 Configuration (Output Divider 22 for 38.4MHz) */
    { 0x3A, 0x00 },
    { 0x3B, 0x01 },
    { 0x3C, 0x00 },
    { 0x3D, 0x09 },
    { 0x3E, 0x00 },
    { 0x3F, 0x00 },
    { 0x40, 0x00 },
    { 0x41, 0x00 },

    /* MS3 Configuration (Output Divider 22 for 38.4MHz) */
    { 0x42, 0x00 },
    { 0x43, 0x01 },
    { 0x44, 0x00 },
    { 0x45, 0x09 },
    { 0x46, 0x00 },
    { 0x47, 0x00 },
    { 0x48, 0x00 },
    { 0x49, 0x00 },

    /* MS4 Configuration (Output Divider 22 for 38.4MHz) */
    { 0x4A, 0x00 },
    { 0x4B, 0x01 },
    { 0x4C, 0x00 },
    { 0x4D, 0x09 },
    { 0x4E, 0x00 },
    { 0x4F, 0x00 },
    { 0x50, 0x00 },
    { 0x51, 0x00 },

    /* MS5 Configuration (Output Divider 22 for 38.4MHz) */
    { 0x52, 0x00 },
    { 0x53, 0x01 },
    { 0x54, 0x00 },
    { 0x55, 0x09 },
    { 0x56, 0x00 },
    { 0x57, 0x00 },
    { 0x58, 0x00 },
    { 0x59, 0x00 },

    /* MS6 Configuration (Output Divider 22 for 38.4MHz) */
    { 0x5A, 0x16 },

    /* MS7 Configuration (Output Divider 22 for 38.4MHz) */
    { 0x5B, 0x16 },

    /* Spread-Spectrum, Fractional Stepping Disabled */
    { 0x95, 0x00 },  // SSDN_P2=0, SSC_EN=0 => no SS
    { 0x96, 0x00 },  // Reserved = 0
    { 0x97, 0x00 },  // SSDN_P3=0, SSC_MODE=0 => off
    { 0x98, 0x00 },  // Reserved = 0
    { 0x99, 0x00 },  // SSDN_P1=0
    { 0x9A, 0x00 },  // SSUDP=0 => no up/down spread
    { 0x9B, 0x00 },  // Reserved = 0

    /* VCXO Configuration */
    { 0xA2, 0xF2 },
    { 0xA3, 0xFD },
    { 0xA4, 0x01 },

    /* Phase Offset Configuration */
    { 0xA5, 0x00 },  // CLK0 phase offset=0
    { 0xA6, 0x00 },  // CLK1 phase offset=0
    { 0xA7, 0x00 },  // CLK2 phase offset=0
    { 0xA8, 0x00 },  // CLK3 phase offset=0
    { 0xA9, 0x00 },  // CLK4 phase offset=0
    { 0xAA, 0x00 },  // CLK5 phase offset=0

    /* Crystal Load Capacitance */
    { 0xB7, 0x12 },  // XTAL_CL=0 (6pF loading)
};

/* SI5351C Default Config from 10MHz ClkIn (38.4MHz on all outputs) */
/*------------------------------------------------------------------*/

const uint8_t si5351_clkin_10m_config[][2] = {
    /* Interrupt Mask Configuration */
    { 0x02, 0x4B },  // Int masks: LOL_B=1, XO_LOS=1, others=0

    /* Output Enable Control */
    { 0x03, 0x00 },  // All CLK outputs enabled via register (OEB pin disabled)

    /* PLL Reset Control */
    { 0x04, 0x20 },  // Disable reset on PLLB LOS (bit5=1), PLLA reset normal

    /* I2C Configuration */
    { 0x07, 0x01 },  // I2C address: 0x60 (default)

    /* Clock Input Configuration */
    { 0x0F, 0x04 },  // PLL source = CLKIN @ 10MHz, CLKIN_DIV=4

    /* Output Channel Configuration (CLK0-CLK7) */
    { 0x10, 0x0F },  // CLK0: LVCMOS 8mA, MS0 src=PLLB
    { 0x11, 0x0F },  // CLK1: LVCMOS 8mA, MS1 src=PLLB
    { 0x12, 0x0F },  // CLK2: LVCMOS 8mA, MS2 src=PLLB
    { 0x13, 0x0F },  // CLK3: LVCMOS 8mA, MS3 src=PLLB
    { 0x14, 0x0F },  // CLK4: LVCMOS 8mA, MS4 src=PLLB
    { 0x15, 0x0F },  // CLK5: LVCMOS 8mA, MS5 src=PLLB
    { 0x16, 0x0F },  // CLK6: LVCMOS 8mA, MS6 src=PLLB
    { 0x17, 0x0F },  // CLK7: LVCMOS 8mA, MS7 src=PLLB

    /* PLLB Configuration (VCO = 844.8MHz from 10MHz input) */
    { 0x1A, 0x00 },
    { 0x1B, 0x19 },
    { 0x1C, 0x00 },
    { 0x1D, 0x28 },
    { 0x1E, 0x3D },
    { 0x1F, 0x00 },
    { 0x20, 0x00 },
    { 0x21, 0x0B },

    /* MS0 Configuration (Output Divider 22 for 38.4MHz) */
    { 0x2A, 0x00 },
    { 0x2B, 0x01 },
    { 0x2C, 0x00 },
    { 0x2D, 0x09 },
    { 0x2E, 0x00 },
    { 0x2F, 0x00 },
    { 0x30, 0x00 },
    { 0x31, 0x00 },

    /* MS1 Configuration (Output Divider 8.448 for 100MHz) */
    { 0x32, 0x00 },
    { 0x33, 0x7D },
    { 0x34, 0x00 },
    { 0x35, 0x02 },
    { 0x36, 0x39 },
    { 0x37, 0x00 },
    { 0x38, 0x00 },
    { 0x39, 0x2B },

    /* MS2 Configuration (Output Divider for 38.4MHz) */
    { 0x3A, 0x00 },
    { 0x3B, 0x01 },
    { 0x3C, 0x00 },
    { 0x3D, 0x09 },
    { 0x3E, 0x00 },
    { 0x3F, 0x00 },
    { 0x40, 0x00 },
    { 0x41, 0x00 },

    /* MS3 Configuration (Output Divider 22 for 38.4MHz) */
    { 0x42, 0x00 },
    { 0x43, 0x01 },
    { 0x44, 0x00 },
    { 0x45, 0x09 },
    { 0x46, 0x00 },
    { 0x47, 0x00 },
    { 0x48, 0x00 },
    { 0x49, 0x00 },

    /* MS4 Configuration (Output Divider 22 for 38.4MHz) */
    { 0x4A, 0x00 },
    { 0x4B, 0x01 },
    { 0x4C, 0x00 },
    { 0x4D, 0x09 },
    { 0x4E, 0x00 },
    { 0x4F, 0x00 },
    { 0x50, 0x00 },
    { 0x51, 0x00 },

    /* MS5 Configuration (Output Divider 22 for 38.4MHz) */
    { 0x52, 0x00 },
    { 0x53, 0x01 },
    { 0x54, 0x00 },
    { 0x55, 0x09 },
    { 0x56, 0x00 },
    { 0x57, 0x00 },
    { 0x58, 0x00 },
    { 0x59, 0x00 },

    /* MS6 Configuration (Output Divider 22 for 38.4MHz) */
    { 0x5A, 0x16 },

    /* MS7 Configuration (Output Divider 22 for 38.4MHz) */
    { 0x5B, 0x16 },

    /* Spread-Spectrum, Fractional Stepping Disabled */
    { 0x95, 0x00 }, // SSDN_P2=0, SSC_EN=0 => no SS
    { 0x96, 0x00 }, // Reserved = 0
    { 0x97, 0x00 }, // SSDN_P3=0, SSC_MODE=0 => off
    { 0x98, 0x00 }, // Reserved = 0
    { 0x99, 0x00 }, // SSDN_P1=0
    { 0x9A, 0x00 }, // SSUDP=0 => no up/down spread
    { 0x9B, 0x00 }, // Reserved = 0

    /* VCXO Configuration */
    { 0xA2, 0x00 },
    { 0xA3, 0x00 },
    { 0xA4, 0x00 },

    /* Phase Offset Configuration */
    { 0xA5, 0x00 }, // CLK0 phase offset=0
    { 0xA6, 0x00 }, // CLK1 phase offset=0
    { 0xA7, 0x00 }, // CLK2 phase offset=0
    { 0xA8, 0x00 }, // CLK3 phase offset=0
    { 0xA9, 0x00 }, // CLK4 phase offset=0
    { 0xAA, 0x00 }, // CLK5 phase offset=0

    /* Crystal Load Capacitance */
    { 0xB7, 0x12 }, // XTAL_CL=0 (6pF loading)
};

/* AD9361 Default Config */
/*-----------------------*/

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
    (int8_t)0xCE, //temp_sense_offset_signed *** adi,temp-sense-offset-signed
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
    1,      //two_t_two_r_timing_enable *** adi,2t2r-timing-enable
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
    RX_CLK_DELAY, //rx_data_clock_delay *** adi,rx-data-clock-delay
    RX_DAT_DELAY, //rx_data_delay *** adi,rx-data-delay
    TX_CLK_DELAY, //tx_fb_clock_delay *** adi,tx-fb-clock-delay
    TX_DAT_DELAY, //tx_data_delay *** adi,tx-data-delay
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

/* AD9361 RX FIR Config */
/*----------------------*/

/* Note: Filter from BladeRF project: https://github.com/Nuand/bladeRF/fpga_common/src/ad936x_params.c */

AD9361_RXFIRConfig rx_fir_config = {
    3,   // rx (RX1 = 1, RX2 = 2, both = 3)
    -6,  // rx_gain (-12, -6, 0, or 6 dB)
    1,   // rx_dec (decimate by 1, 2, or 4)

    /**
     * RX FIR Filter
     * Built using https://github.com/analogdevicesinc/libad9361-iio
     * Branch: filter_generation
     * Commit: f749cef974f687f696226455dc7684277886cf3b
     *
     * This filter is intended to improve the flatness of the RX spectrum. It is
     * a 64-tap, decimate-by-1 filter.
     *
     * Design parameters:
     *
     * fdp.Rdata = 30720000;
     * fdp.RxTx = "Rx";
     * fdp.Type = "Lowpass";
     * fdp.DAC_div = 1;
     * fdp.HB3 = 2;
     * fdp.HB2 = 2;
     * fdp.HB1 = 2;
     * fdp.FIR = 1;
     * fdp.PLL_mult = 4;
     * fdp.converter_rate = 245760000;
     * fdp.PLL_rate = 983040000;
     * fdp.Fpass = fdp.Rdata*0.42;
     * fdp.Fstop = fdp.Rdata*0.50;
     * fdp.Fcenter = 0;
     * fdp.Apass = 0.125;
     * fdp.Astop = 85;
     * fdp.phEQ = -1;
     * fdp.wnom = 17920000;
     * fdp.caldiv = 7;
     * fdp.RFbw = 22132002;
     * fdp.FIRdBmin = 0;
     * fdp.int_FIR = 1;
     */
    // clang-format off
    {
          0,      0,      0,      1,     -1,      3,     -6,     11,
        -19,     33,    -53,     84,   -129,    193,   -282,    404,
       -565,    777,  -1052,   1401,  -1841,   2390,  -3071,   3911,
      -4947,   6230,  -7833,   9888, -12416,  15624, -21140,  32767,
      32767, -21140,  15624, -12416,   9888,  -7833,   6230,  -4947,
       3911,  -3071,   2390,  -1841,   1401,  -1052,    777,   -565,
        404,   -282,    193,   -129,     84,    -53,     33,    -19,
         11,     -6,      3,     -1,      1,      0,      0,      0,
          0,      0,      0,      0,      0,      0,      0,      0, // unused
          0,      0,      0,      0,      0,      0,      0,      0, // unused
          0,      0,      0,      0,      0,      0,      0,      0, // unused
          0,      0,      0,      0,      0,      0,      0,      0, // unused
          0,      0,      0,      0,      0,      0,      0,      0, // unused
          0,      0,      0,      0,      0,      0,      0,      0, // unused
          0,      0,      0,      0,      0,      0,      0,      0, // unused
          0,      0,      0,      0,      0,      0,      0,      0, // unused
    }, // rx_coef[128]
    // clang-format on
    64,                    // rx_coef_size
    { 0, 0, 0, 0, 0, 0 },  // rx_path_clks[6]
    0                      // rx_bandwidth
};

/* AD9361 TX FIR Config */
/*----------------------*/

/* Note: Filter from BladeRF project: https://github.com/Nuand/bladeRF/fpga_common/src/ad936x_params.c */

AD9361_TXFIRConfig tx_fir_config = {
    3,  // tx (TX1 = 1, TX2 = 2, both = 3)
    0,  // tx_gain (-6 or 0 dB)
    1,  // tx_int (interpolate by 1, 2, or 4)

    /**
     * TX FIR Filter
     *
     * This filter literally does nothing, but it is here as a placeholder.
     */
    // clang-format off
    {
      32767,      0,      0,      0,      0,      0,      0,      0,
          0,      0,      0,      0,      0,      0,      0,      0,
          0,      0,      0,      0,      0,      0,      0,      0,
          0,      0,      0,      0,      0,      0,      0,      0,
          0,      0,      0,      0,      0,      0,      0,      0,
          0,      0,      0,      0,      0,      0,      0,      0,
          0,      0,      0,      0,      0,      0,      0,      0,
          0,      0,      0,      0,      0,      0,      0,      0,
          0,      0,      0,      0,      0,      0,      0,      0, // unused
          0,      0,      0,      0,      0,      0,      0,      0, // unused
          0,      0,      0,      0,      0,      0,      0,      0, // unused
          0,      0,      0,      0,      0,      0,      0,      0, // unused
          0,      0,      0,      0,      0,      0,      0,      0, // unused
          0,      0,      0,      0,      0,      0,      0,      0, // unused
          0,      0,      0,      0,      0,      0,      0,      0, // unused
          0,      0,      0,      0,      0,      0,      0,      0, // unused
    }, // tx_coef[128]
    // clang-format on
    64,                    // tx_coef_size
    { 0, 0, 0, 0, 0, 0 },  // tx_path_clks[6]
    0                      // tx_bandwidth
};
