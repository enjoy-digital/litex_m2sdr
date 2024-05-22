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

#include "ad9361/platform.h"
#include "ad9361/ad9361.h"
#include "ad9361/ad9361_api.h"

#include "liblitepcie.h"
#include "libm2sdr.h"

/* Parameters */
/*------------*/

/* Variables */
/*-----------*/

static char litepcie_device[1024];
static int litepcie_device_num;

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

bool gpio_is_valid(struct gpio_device *gpio_dev, int number)
{
    switch(number) {
    case AD9361_GPIO_RESET_PIN:
        return true;
    default:
        return false;
    }
}

void gpio_set_value(struct gpio_device *gpio_dev, unsigned gpio, int value)
{
    /* FIXME: Implement */
}

AD9361_InitParam default_init_param = {
    /* Reference Clock */
    38400000UL, //reference_clk_rate
    /* Base Configuration */
    1,      //two_rx_two_tx_mode_enable *** adi,2rx-2tx-mode-enable
    1,      //frequency_division_duplex_mode_enable *** adi,frequency-division-duplex-mode-enable
    0,      //tdd_use_dual_synth_mode_enable *** adi,tdd-use-dual-synth-mode-enable
    0,      //tdd_skip_vco_cal_enable *** adi,tdd-skip-vco-cal-enable
    0,      //tx_fastlock_delay_ns *** adi,tx-fastlock-delay-ns
    0,      //rx_fastlock_delay_ns *** adi,rx-fastlock-delay-ns
    0,      //rx_fastlock_pincontrol_enable *** adi,rx-fastlock-pincontrol-enable
    0,      //tx_fastlock_pincontrol_enable *** adi,tx-fastlock-pincontrol-enable
    0,      //external_rx_lo_enable *** adi,external-rx-lo-enable
    0,      //external_tx_lo_enable *** adi,external-tx-lo-enable
    5,      //dc_offset_tracking_update_event_mask *** adi,dc-offset-tracking-update-event-mask
    6,      //dc_offset_attenuation_high_range *** adi,dc-offset-tracking-update-event-mask
    5,      //dc_offset_attenuation_low_range *** adi,dc-offset-tracking-update-event-mask
    0x28,   //dc_offset_count_high_range *** adi,dc-offset-tracking-update-event-mask
    0x32,   //dc_offset_count_low_range *** adi,dc-offset-tracking-update-event-mask
    0,      //tdd_use_fdd_vco_tables_enable *** adi,tdd-use-fdd-vco-tables-enable
    0,      //split_gain_table_mode_enable *** adi,split-gain-table-mode-enable
    /* ENSM Control */
    0,      //ensm_enable_pin_pulse_mode_enable *** adi,ensm-enable-pin-pulse-mode-enable
    0,      //ensm_enable_txnrx_control_enable *** adi,ensm-enable-txnrx-control-enable
    /* LO Control */
    2400000000UL,   //rx_synthesizer_frequency_hz *** adi,rx-synthesizer-frequency-hz
    2400000000UL,   //tx_synthesizer_frequency_hz *** adi,tx-synthesizer-frequency-hz
    /* Rate & BW Control */
    {983040000, 245760000, 122880000, 61440000, 30720000, 30720000},//uint32_t  rx_path_clock_frequencies[6] *** adi,rx-path-clock-frequencies
    {983040000, 122880000, 122880000, 61440000, 30720000, 30720000},//uint32_t  tx_path_clock_frequencies[6] *** adi,tx-path-clock-frequencies
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
    0,      //clk_output_mode_select *** adi,clk-output-mode-select
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
        /* Fast AGC - Lock Level */
    10,     //fagc_lock_level ***  adi,fagc-lock-level */
    1,      //fagc_lock_level_lmt_gain_increase_en ***  adi,fagc-lock-level-lmt-gain-increase-enable
    5,      //fagc_lock_level_gain_increase_upper_limit ***  adi,fagc-lock-level-gain-increase-upper-limit
        /* Fast AGC - Peak Detectors and Final Settling */
    1,      //fagc_lpf_final_settling_steps ***  adi,fagc-lpf-final-settling-steps
    1,      //fagc_lmt_final_settling_steps ***  adi,fagc-lmt-final-settling-steps
    3,      //fagc_final_overrange_count ***  adi,fagc-final-overrange-count
        /* Fast AGC - Final Power Test */
    0,      //fagc_gain_increase_after_gain_lock_en ***  adi,fagc-gain-increase-after-gain-lock-enable
        /* Fast AGC - Unlocking the Gain */
        /* 0 = MAX Gain, 1 = Optimized Gain, 2 = Set Gain */
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
    /* Digital Interface Control */
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
    0,      //rx_data_clock_delay *** adi,rx-data-clock-delay
    4,      //rx_data_delay *** adi,rx-data-delay
    7,      //tx_fb_clock_delay *** adi,tx-fb-clock-delay
    0,      //tx_data_delay *** adi,tx-data-delay
    150,    //lvds_bias_mV *** adi,lvds-bias-mV
    1,      //lvds_rx_onchip_termination_enable *** adi,lvds-rx-onchip-termination-enable
    0,      //rx1rx2_phase_inversion_en *** adi,rx1-rx2-phase-inversion-enable
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
    -1,     //gpio_resetb;  /* reset-gpios */
    /* MCS Sync */
    -1,     //gpio_sync;        /* sync-gpios */
    -1,     //gpio_cal_sw1; /* cal-sw1-gpios */
    -1      //gpio_cal_sw2; /* cal-sw2-gpios */
};

AD9361_RXFIRConfig rx_fir_config = {
    3, // rx;
    0, // rx_gain;
    1, // rx_dec;
    {-4, -6, -37, 35, 186, 86, -284, 315,
     107, 219, -4, 271, 558, -307, -1182, -356,
     658, 157, 207, 1648, 790, -2525, -2553, 748,
     865, -476, 3737, 6560, -3583, -14731, -5278, 14819,
     14819, -5278, -14731, -3583, 6560, 3737, -476, 865,
     748, -2553, -2525, 790, 1648, 207, 157, 658,
     -356, -1182, -307, 558, 271, -4, 219, 107,
     -315, -284, 86, 186, 35, -37, -6, -4} // rx_coef[64];
};

AD9361_TXFIRConfig tx_fir_config = {
    3, // tx;
    -6, // tx_gain;
    1, // tx_inc;
    {-4, -6, -37, 35, 186, 86, -284, 315,
     107, 219, -4, 271, 558, -307, -1182, -356,
     658, 157, 207, 1648, 790, -2525, -2553, 748,
     865, -476, 3737, 6560, -3583, -14731, -5278, 14819,
     14819, -5278, -14731, -3583, 6560, 3737, -476, 865,
     748, -2553, -2525, 790, 1648, 207, 157, 658,
     -356, -1182, -307, 558, 271, -4, 219, 107,
     -315, -284, 86, 186, 35, -37, -6, -4} // tx_coef[64];
};

/* Info */
/*------*/

static void info(void)
{
    int fd;
    int i;
    unsigned char fpga_identifier[256];

    fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }


    printf("\e[1m[> FPGA/SoC Info:\e[0m\n");
    printf("---------------------\n");

    for (i = 0; i < 256; i ++)
        fpga_identifier[i] = litepcie_readl(fd, CSR_IDENTIFIER_MEM_BASE + 4 * i);
    printf("FPGA Identifier:  %s.\n", fpga_identifier);
#ifdef CSR_DNA_BASE
    printf("FPGA DNA:         0x%08x%08x\n",
        litepcie_readl(fd, CSR_DNA_ID_ADDR + 4 * 0),
        litepcie_readl(fd, CSR_DNA_ID_ADDR + 4 * 1)
    );
#endif
#ifdef CSR_XADC_BASE
    printf("FPGA Temperature : %0.1f °C\n",
           (double)litepcie_readl(fd, CSR_XADC_TEMPERATURE_ADDR) * 503.975/4096 - 273.15);
    printf("FPGA VCC-INT     : %0.2f V\n",
           (double)litepcie_readl(fd, CSR_XADC_VCCINT_ADDR) / 4096 * 3);
    printf("FPGA VCC-AUX     : %0.2f V\n",
           (double)litepcie_readl(fd, CSR_XADC_VCCAUX_ADDR) / 4096 * 3);
    printf("FPGA VCC-BRAM    : %0.2f V\n",
           (double)litepcie_readl(fd, CSR_XADC_VCCBRAM_ADDR) / 4096 * 3);
#endif
    printf("\n");

    close(fd);
}

/* Init */
/*------*/

//#define BIST_TONE
#define BIST_PRBS

static void init(void)
{
    int fd;

    fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }

    default_init_param.gpio_resetb = AD9361_GPIO_RESET_PIN;
    default_init_param.gpio_sync = -1;
    default_init_param.gpio_cal_sw1 = -1;
    default_init_param.gpio_cal_sw2 = -1;

    m2sdr_ad9361_spi_init(fd);

    ad9361_phy = ad9361_init(&default_init_param);

    ad9361_set_tx_fir_config(ad9361_phy, tx_fir_config);
    ad9361_set_rx_fir_config(ad9361_phy, rx_fir_config);

    printf("SPI Register 0x010—Parallel Port Configuration 1: %08x\n", m2sdr_ad9361_spi_read(fd, 0x10));
    printf("SPI Register 0x011—Parallel Port Configuration 2: %08x\n", m2sdr_ad9361_spi_read(fd, 0x11));
    printf("SPI Register 0x012—Parallel Port Configuration 3: %08x\n", m2sdr_ad9361_spi_read(fd, 0x12));

    printf("AD9361 Control: %08x\n", litepcie_readl(fd, CSR_AD9361_CONFIG_ADDR));

    ad9361_bist_loopback(ad9361_phy, 0);
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
    printf("M2SDR utilities\n"
           "usage: m2sdr_rf [options] cmd [args...]\n"
           "\n"
           "options:\n"
           "-h                                Help.\n"
           "-c device_num                     Select the device (default = 0).\n"
           "\n"
           "available commands:\n"
           "info                              Get Board information.\n"
           "init                              Init AD9361/RF.\n"
           );
    exit(1);
}

/* Main */
/*------*/

int main(int argc, char **argv)
{
    const char *cmd;
    int c;

    litepcie_device_num = 0;

    /* Parameters. */
    for (;;) {
        c = getopt(argc, argv, "hc:w:zea");
        if (c == -1)
            break;
        switch(c) {
        case 'h':
            help();
            break;
        case 'c':
            litepcie_device_num = atoi(optarg);
            break;
        default:
            exit(1);
        }
    }

    /* Show help when too much args. */
    if (optind >= argc)
        help();

    /* Select device. */
    snprintf(litepcie_device, sizeof(litepcie_device), "/dev/litepcie%d", litepcie_device_num);

    cmd = argv[optind++];

    /* Info cmds. */
    if (!strcmp(cmd, "info"))
        info();
    /* Init cmds. */
    if (!strcmp(cmd, "init"))
        init();
    /* Show help otherwise. */
    else
        goto show_help;

    return 0;

show_help:
        help();

    return 0;
}
