/*  Copyright (C) 2020 NANDO authors
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 */

#ifndef _FSMC_NAND_H_
#define _FSMC_NAND_H_

#include "flash_hal.h"

typedef struct __attribute__((__packed__))
{
    uint8_t setup_time;
    uint8_t wait_setup_time;
    uint8_t hold_setup_time;
    uint8_t hi_z_setup_time;
    uint8_t clr_setup_time;
    uint8_t ar_setup_time;
    uint8_t row_cycles;
    uint8_t col_cycles;
    uint8_t read1_cmd;
    uint8_t read2_cmd;
    uint8_t read_spare_cmd;
    uint8_t read_id_cmd;
    uint8_t reset_cmd;
    uint8_t write1_cmd;
    uint8_t write2_cmd;
    uint8_t erase1_cmd;
    uint8_t erase2_cmd;
    uint8_t status_cmd;
    uint8_t set_features_cmd;
    uint8_t enable_ecc_addr;
    uint8_t enable_ecc_value;
    uint8_t disable_ecc_value;
} fsmc_conf_t;

typedef struct __attribute__((__packed__))
{
    uint32_t signature;
    uint16_t revision;
    uint16_t features;
    uint16_t optional_cmds;
    uint8_t  reserved1[22];
    char     manufacturer[12];
    char     model[20];
    uint8_t  jedec_id;
    uint8_t  reserved2[15];
    uint32_t page_data_bytes;
    uint16_t page_spare_bytes;
    uint32_t partial_page_data_bytes;
    uint16_t partial_page_spare_bytes;
    uint32_t pages_per_block;
    uint32_t blocks_per_lun;
    uint8_t  lun_count;
    uint8_t  address_cycles;
    uint8_t  bits_per_cell;
    uint16_t max_bad_blocks_per_lun;
    uint16_t block_endurance;
    uint8_t  reserved3[21];
    uint16_t async_timing_modes;
    uint8_t  reserved4[124];
    uint16_t integrity_crc;
} onfi_param_page_t;

extern flash_hal_t hal_fsmc;

int fsmc_nand_read_onfi(onfi_param_page_t *onfi_out);

#endif /* _FSMC_NAND_H_ */
