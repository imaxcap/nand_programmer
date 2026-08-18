/*  Copyright (C) 2026 Qualcomm MIBIB & NAND Programmer authors
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 */

#ifndef _QPIC_H_
#define _QPIC_H_

#include <stdint.h>
#include <stdbool.h>

#define QPIC_CW_DATA_BYTES   516
#define QPIC_USER_STEP_BYTES 512
#define QPIC_BCH4_CW_SIZE    528
#define QPIC_BCH8_CW_SIZE    532
#define QPIC_BCH4_PARITY_LEN 7
#define QPIC_BCH8_PARITY_LEN 13

typedef enum
{
    QPIC_ECC_NONE = 0,
    QPIC_ECC_BCH4 = 4,
    QPIC_ECC_BCH8 = 8,
} qpic_ecc_mode_t;

void qpic_init(void);

void qpic_encode_bch4(const uint8_t *data, uint32_t len, uint8_t *parity_out);
void qpic_encode_bch8(const uint8_t *data, uint32_t len, uint8_t *parity_out);

/**
 * @brief Interleave flat user data into Qualcomm QPIC Codeword formatted page with BBM & BCH parity.
 * 
 * @param flat_in Pointer to flat user data (pageSize bytes)
 * @param raw_out Output buffer for raw interleaved NAND page (pageSize + oobSize bytes)
 * @param page_size NAND page data size (e.g. 2048 or 4096)
 * @param oob_size NAND OOB/spare size (e.g. 64, 128, 256)
 * @param ecc_mode BCH4 or BCH8
 * @return int 0 on success, negative error code on failure
 */
int qpic_interleave_page(const uint8_t *flat_in, uint8_t *raw_out, uint32_t page_size, uint32_t oob_size, qpic_ecc_mode_t ecc_mode);

/**
 * @brief De-interleave raw Qualcomm QPIC page into contiguous flat user data.
 * 
 * @param raw_in Pointer to raw NAND page (pageSize + oobSize bytes)
 * @param flat_out Output buffer for contiguous user data (pageSize bytes)
 * @param page_size NAND page data size (e.g. 2048 or 4096)
 * @param oob_size NAND OOB/spare size (e.g. 64, 128, 256)
 * @param ecc_mode BCH4 or BCH8
 * @return int 0 on success, negative error code on failure
 */
int qpic_deinterleave_page(const uint8_t *raw_in, uint8_t *flat_out, uint32_t page_size, uint32_t oob_size, qpic_ecc_mode_t ecc_mode);

#endif
