/*  Copyright (C) 2020 NANDO authors
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 */

#include "nand_programmer.h"
#include "nand_bad_block.h"
#include "fsmc_nand.h"
#include "chip_info.h"
#include "led.h"
#include "log.h"
#include "version.h"
#include "flash.h"
#include "spi_flash.h"
#include "qpic.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <inttypes.h>

#define NP_PACKET_BUF_SIZE 64
#define NP_MAX_PAGE_SIZE 0x21C0 /* 8KB + 448 spare */
#define NP_WRITE_ACK_BYTES 1984
#define NP_NAND_TIMEOUT 0x1000000

#define NP_NAND_GOOD_BLOCK_MARK 0xFF

#define BOOT_CONFIG_ADDR 0x08003800
#define FLASH_START_ADDR 0x08000000
#define FLASH_SIZE 0x40000
#define FLASH_PAGE_SIZE 0x800
#define FLASH_BLOCK_SIZE 0x800

typedef enum
{
    NP_CMD_NAND_READ_ID     = 0x00,
    NP_CMD_NAND_ERASE       = 0x01,
    NP_CMD_NAND_READ        = 0x02,
    NP_CMD_NAND_WRITE_S     = 0x03,
    NP_CMD_NAND_WRITE_D     = 0x04,
    NP_CMD_NAND_WRITE_E     = 0x05,
    NP_CMD_NAND_CONF        = 0x06,
    NP_CMD_NAND_READ_BB     = 0x07,
    NP_CMD_VERSION_GET      = 0x08,
    NP_CMD_ACTIVE_IMAGE_GET = 0x09,
    NP_CMD_FW_UPDATE_S      = 0x0a,
    NP_CMD_FW_UPDATE_D      = 0x0b,
    NP_CMD_FW_UPDATE_E      = 0x0c,
    NP_CMD_NAND_SCRUB       = 0x10,
    NP_CMD_NAND_TEST        = 0x11,
    NP_CMD_NAND_PROBE_ONFI  = 0x12,
    NP_CMD_NAND_LAST        = 0x13,
} np_cmd_code_t;

enum
{
    NP_ERR_INTERNAL       = -1,
    NP_ERR_ADDR_EXCEEDED  = -100,
    NP_ERR_ADDR_INVALID   = -101,
    NP_ERR_ADDR_NOT_ALIGN = -102,
    NP_ERR_NAND_WR        = -103,
    NP_ERR_NAND_RD        = -104,
    NP_ERR_NAND_ERASE     = -105,
    NP_ERR_CHIP_NOT_CONF  = -106,
    NP_ERR_CMD_DATA_SIZE  = -107,
    NP_ERR_CMD_INVALID    = -108,
    NP_ERR_BUF_OVERFLOW   = -109,
    NP_ERR_LEN_NOT_ALIGN  = -110,
    NP_ERR_LEN_EXCEEDED   = -111,
    NP_ERR_LEN_INVALID    = -112,
    NP_ERR_BBT_OVERFLOW   = -113,
    NP_ERR_TEST_FAIL      = -114,
};

typedef struct __attribute__((__packed__))
{
    np_cmd_code_t code;
} np_cmd_t;

typedef struct __attribute__((__packed__))
{
    uint8_t skip_bb : 1;
    uint8_t inc_spare : 1;
    uint8_t enable_hw_ecc: 1;
    uint8_t qpic_bch4 : 1;
    uint8_t qpic_bch8 : 1;
    uint8_t reserved : 3;
} np_cmd_flags_t;

typedef struct __attribute__((__packed__))
{
    np_cmd_t cmd;
    uint64_t addr;
    uint64_t len;
    np_cmd_flags_t flags;
} np_erase_cmd_t;

typedef struct __attribute__((__packed__))
{
    np_cmd_t cmd;
    uint64_t addr;
    uint64_t len;
} np_scrub_cmd_t;

enum
{
    NP_TEST_MODE_FULL_BLOCK  = 0,
    NP_TEST_MODE_WRITE_ONLY  = 1,
    NP_TEST_MODE_VERIFY_ONLY = 2,
    NP_TEST_MODE_FULL_CHIP   = 3,
};

typedef struct __attribute__((__packed__))
{
    np_cmd_t cmd;
    uint64_t addr;
    uint64_t len;
    uint8_t  mode;
    uint8_t  mark_bad;
    uint32_t seed;
} np_test_cmd_t;

typedef struct __attribute__((__packed__))
{
    np_cmd_t cmd;
    uint64_t addr;
    uint64_t len;
    np_cmd_flags_t flags;
} np_write_start_cmd_t;

typedef struct __attribute__((__packed__))
{
    np_cmd_t cmd;
    uint8_t len;
    uint8_t data[];
} np_write_data_cmd_t;

typedef struct __attribute__((__packed__))
{
    np_cmd_t cmd;
} np_write_end_cmd_t;

typedef struct __attribute__((__packed__))
{
    np_cmd_t cmd;
    uint64_t addr;
    uint64_t len;
    np_cmd_flags_t flags;
} np_read_cmd_t;

typedef struct __attribute__((__packed__))
{
    np_cmd_t cmd;
    uint8_t hal;
    uint32_t page_size;
    uint32_t block_size;
    uint64_t total_size;
    uint32_t spare_size;    
    uint8_t bb_mark_off;
    uint8_t hal_conf[];
} np_conf_cmd_t;

enum
{
    NP_RESP_DATA   = 0x00,
    NP_RESP_STATUS = 0x01,
};

typedef struct __attribute__((__packed__))
{
    uint8_t code;
    uint8_t info;
    uint8_t data[];
} np_resp_t;

enum
{
    NP_STATUS_OK        = 0x00,
    NP_STATUS_ERROR     = 0x01,
    NP_STATUS_BB        = 0x02,
    NP_STATUS_WRITE_ACK = 0x03,
    NP_STATUS_BB_SKIP   = 0x04,
    NP_STATUS_PROGRESS  = 0x05,
};

typedef struct __attribute__((__packed__))
{
    np_resp_t header;
    chip_id_t nand_id;
} np_resp_id_t;

/* BB, write ack and error responses are aligned to the same size to avoid
 * receiver wait for additional data */
typedef struct __attribute__((__packed__))
{
    np_resp_t header;
    uint64_t addr;
    uint32_t size;
} np_resp_bad_block_t;

typedef struct __attribute__((__packed__))
{
    np_resp_t header;
    uint64_t bytes_ack;
    uint8_t dummy[4];
} np_resp_write_ack_t;

typedef struct __attribute__((__packed__))
{
    np_resp_t header;
    uint8_t err_code;
    uint8_t dummy[11];
} np_resp_err_t;

typedef struct __attribute__((__packed__))
{
    np_resp_t header;
    uint64_t progress;
} np_resp_progress_t;

typedef struct __attribute__((__packed__))
{
    uint8_t major;
    uint8_t minor;
    uint16_t build;
} version_t;

typedef struct __attribute__((__packed__))
{
    np_resp_t header;
    version_t version;
} np_resp_version_t;

typedef struct __attribute__((__packed__))
{
    np_resp_t header;
    uint8_t active_image;
} np_resp_active_image_t;

typedef struct __attribute__((__packed__))
{
    np_resp_t header;
    char manufacturer[12];
    char model[20];
    uint32_t page_size;
    uint32_t block_size;
    uint64_t total_size;
    uint32_t spare_size;
    uint8_t  row_cycles;
    uint8_t  col_cycles;
} np_resp_onfi_t;

typedef struct
{
    uint32_t addr;
    int is_valid;
} np_prog_addr_t;

typedef struct
{
    uint8_t buf[NP_MAX_PAGE_SIZE];
    uint32_t page;
    uint32_t offset;
} np_page_t;

typedef struct __attribute__((__packed__))
{
    uint8_t active_image;
} boot_config_t;

typedef struct
{
    uint8_t *rx_buf;
    uint32_t rx_buf_len;
    uint64_t addr;
    uint64_t len;
    uint64_t base_addr;
    uint32_t page_size;
    uint32_t block_size;
    uint64_t total_size;
    int addr_is_set;
    int bb_is_read;
    int chip_is_conf;
    np_page_t page;
    uint64_t bytes_written;
    uint64_t bytes_ack;
    int skip_bb;
    int nand_wr_in_progress;
    uint32_t nand_timeout;
    chip_info_t chip_info;
    uint8_t active_image;
    uint8_t hal;
    uint8_t qpic_ecc;
} np_prog_t;

typedef struct
{
    int id;
    bool is_chip_cmd;
    int (*exec)(np_prog_t *prog);
} np_cmd_handler_t;

static np_comm_cb_t *np_comm_cb;
static np_prog_t prog;

static flash_hal_t *hal[] = { &hal_fsmc, &hal_spi };
static int np_nand_handle_status(np_prog_t *prog);
static uint8_t qpic_raw_buf[NP_MAX_PAGE_SIZE];

uint8_t np_packet_send_buf[NP_PACKET_BUF_SIZE];

static int np_send_ok_status()
{
    np_resp_t status = { NP_RESP_STATUS, NP_STATUS_OK };
    size_t len = sizeof(status);

    if (np_comm_cb)
        np_comm_cb->send((uint8_t *)&status, len);

    return 0;
}

static int np_send_error(uint8_t err_code)
{
    np_resp_t status = { NP_RESP_STATUS, NP_STATUS_ERROR };
    np_resp_err_t err_status = { status, err_code };
    size_t len = sizeof(err_status);

    if (np_comm_cb)
        np_comm_cb->send((uint8_t *)&err_status, len);

    return 0;
}

static int np_send_bad_block_info(uint64_t addr, uint32_t size, bool is_skipped)
{
    uint8_t info = is_skipped ? NP_STATUS_BB_SKIP : NP_STATUS_BB;
    np_resp_t resp_header = { NP_RESP_STATUS, info };
    np_resp_bad_block_t bad_block = { resp_header, addr, size };

    if (np_comm_cb->send((uint8_t *)&bad_block, sizeof(bad_block)))
        return -1;

    return 0;
}

static int np_send_progress(uint64_t progress)
{
    np_resp_t resp_header = { NP_RESP_STATUS, NP_STATUS_PROGRESS };
    np_resp_progress_t resp_progress = { resp_header, progress };

    if (np_comm_cb->send((uint8_t *)&resp_progress, sizeof(resp_progress)))
        return -1;

    return 0;
}

static int _np_cmd_nand_read_id(np_prog_t *prog)
{
    np_resp_id_t resp;
    size_t resp_len = sizeof(resp);

    DEBUG_PRINT("Read ID command\r\n");

    resp.header.code = NP_RESP_DATA;
    resp.header.info = resp_len - sizeof(resp.header);
    hal[prog->hal]->read_id(&resp.nand_id);

    if (np_comm_cb)
        np_comm_cb->send((uint8_t *)&resp, resp_len);

    DEBUG_PRINT("Chip ID: 0x%x 0x%x 0x%x 0x%x 0x%x\r\n",
        resp.nand_id.maker_id, resp.nand_id.device_id, resp.nand_id.third_id,
        resp.nand_id.fourth_id, resp.nand_id.fifth_id);

    return 0;
}

static int np_cmd_nand_read_id(np_prog_t *prog)
{
    int ret;

    led_rd_set(true);
    ret = _np_cmd_nand_read_id(prog);
    led_rd_set(false);

    return ret;
}

static int np_read_bad_block_info_from_page(np_prog_t *prog, uint32_t block,
    uint32_t page, bool *is_bad)
{
    uint32_t status;
    uint64_t addr = block * prog->chip_info.block_size;
    uint8_t *bb_mark = &prog->page.buf[prog->chip_info.page_size +
        prog->chip_info.bb_mark_off];

    status = hal[prog->hal]->read_spare_data(bb_mark, page,
        prog->chip_info.bb_mark_off, 1);
    if (status == FLASH_STATUS_INVALID_CMD)
    {
        status = hal[prog->hal]->read_page(prog->page.buf, page,
            prog->chip_info.page_size + prog->chip_info.spare_size);
    }

    switch (status)
    {
    case FLASH_STATUS_READY:
        break;
    case FLASH_STATUS_ERROR:
        ERROR_PRINT("NAND read bad block info error at 0x%llx" "\r\n",
            (unsigned long long)addr);
        return NP_ERR_NAND_RD;
    case FLASH_STATUS_TIMEOUT:
        ERROR_PRINT("NAND read timeout at 0x%llx" "\r\n", (unsigned long long)addr);
        return NP_ERR_NAND_RD;
    default:
        ERROR_PRINT("Unknown NAND status\r\n");
        return NP_ERR_NAND_RD;
    }

    *is_bad = prog->page.buf[prog->chip_info.page_size +
        prog->chip_info.bb_mark_off] != NP_NAND_GOOD_BLOCK_MARK;

    return 0;
}

static int _np_cmd_read_bad_blocks(np_prog_t *prog, bool send_progress)
{
    int ret;
    bool is_bad;
    uint32_t block, block_num, page_num, page;

    if (!hal[prog->hal]->is_bb_supported())
        goto Exit;

    block_num = prog->chip_info.total_size / prog->chip_info.block_size;
    page_num = prog->chip_info.block_size / prog->chip_info.page_size;

    /* Bad block - not 0xFF value in the first or second page in the block at
     * some offset in the page spare area
     */
    for (block = 0; block < block_num; block++)
    {
        page = block * page_num;

        if (send_progress)
            np_send_progress(page);

        if ((ret = np_read_bad_block_info_from_page(prog, block, page,
            &is_bad)))
        {
            return ret;
        }

        if (!is_bad && (ret = np_read_bad_block_info_from_page(prog, block,
            page + 1, &is_bad)))
        {
            return ret;
        }

        if (is_bad && nand_bad_block_table_add(page))
            return NP_ERR_BBT_OVERFLOW;
    }

Exit:
    prog->bb_is_read = 1;

    return 0;
}

static int np_nand_erase(np_prog_t *prog, uint32_t page)
{
    uint32_t status;
    uint64_t addr = page * prog->chip_info.page_size;
    
    DEBUG_PRINT("NAND erase at 0x%llx" "\r\n", (unsigned long long)addr);

    status = hal[prog->hal]->erase_block(page);
    switch (status)
    {
    case FLASH_STATUS_READY:
        break;
    case FLASH_STATUS_ERROR:
        if (np_send_bad_block_info(addr, prog->chip_info.block_size, false))
            return -1;
        break;
    case FLASH_STATUS_TIMEOUT:
        ERROR_PRINT("NAND erase timeout at 0x%llx" "\r\n", (unsigned long long)addr);
        break;
    default:
        ERROR_PRINT("Unknown NAND status\r\n");
        return -1;
    }

    return 0;
}

static int _np_cmd_nand_erase(np_prog_t *prog)
{
    int ret;
    uint64_t addr, len, total_size, total_len;
    uint32_t page, pages, pages_in_block, page_size, block_size;
    np_erase_cmd_t *erase_cmd;
    bool skip_bb, inc_spare, is_bad = false;

    if (prog->rx_buf_len < sizeof(np_erase_cmd_t))
    {
        ERROR_PRINT("Wrong buffer length for erase command %lu\r\n",
            prog->rx_buf_len);
        return NP_ERR_LEN_INVALID;
    }
    erase_cmd = (np_erase_cmd_t *)prog->rx_buf;
    total_len = len = erase_cmd->len;
    addr = erase_cmd->addr;
    skip_bb = erase_cmd->flags.skip_bb;
    inc_spare = erase_cmd->flags.inc_spare;

    DEBUG_PRINT("Erase at 0x%llx 0x%llx" " bytes command\r\n", (unsigned long long)addr,
        len);

    pages_in_block = prog->chip_info.block_size / prog->chip_info.page_size;

    if (inc_spare)
    {
        pages = prog->chip_info.total_size / prog->chip_info.page_size;
        page_size = prog->chip_info.page_size + prog->chip_info.spare_size;
        block_size = pages_in_block * page_size;
        total_size = (uint64_t)pages * page_size;
    }
    else
    {
        page_size = prog->chip_info.page_size;
        block_size = prog->chip_info.block_size;
        total_size = prog->chip_info.total_size;
    }

    if (skip_bb && !prog->bb_is_read && (ret = _np_cmd_read_bad_blocks(prog,
        false)))
    {
        return ret;
    }

    if (addr % block_size)
    {
        ERROR_PRINT("Address 0x%llx"
            " is not aligned to block size 0x%lx\r\n", (unsigned long long)addr, block_size);
        return NP_ERR_ADDR_NOT_ALIGN;
    }

    if (!len)
    {
        ERROR_PRINT("Length is 0\r\n");
        return NP_ERR_LEN_INVALID;
    }

    if (len % block_size)
    {
        ERROR_PRINT("Length 0x%llx"
            " is not aligned to block size 0x%lx\r\n", (unsigned long long)len, block_size);
        return NP_ERR_LEN_NOT_ALIGN;
    }

    if (addr + len > total_size)
    {
        ERROR_PRINT("Erase address exceded 0x%llx+0x%llx"
            " is more then chip size 0x%llx" "\r\n", (unsigned long long)addr, (unsigned long long)len, (unsigned long long)total_size);
        return NP_ERR_ADDR_EXCEEDED;
    }

    page = addr / page_size;

    while (len)
    {
        if (addr >= total_size)
        {
            ERROR_PRINT("Erase address 0x%llx"
                " is more then chip size 0x%llx" "\r\n", (unsigned long long)addr, (unsigned long long)total_size);
            return NP_ERR_ADDR_EXCEEDED;
        }

        if (skip_bb && (is_bad = nand_bad_block_table_lookup(page)))
        {
            DEBUG_PRINT("Skipped bad block at 0x%llx" "\r\n", (unsigned long long)addr);
            if (np_send_bad_block_info(addr, block_size, true))
                return -1;
        }

        if (!is_bad && np_nand_erase(prog, page))
            return NP_ERR_NAND_ERASE;

        addr += block_size;
        page += pages_in_block;
        /* On partial erase do not count bad blocks */
        if (!is_bad || (is_bad && erase_cmd->len == total_size))
            len -= block_size;

        np_send_progress(total_len - len);
    }

    return np_send_ok_status();
}

static int np_cmd_nand_erase(np_prog_t *prog)
{
    int ret;

    led_wr_set(true);
    ret = _np_cmd_nand_erase(prog);
    led_wr_set(false);

    return ret;
}

static int _np_cmd_nand_scrub(np_prog_t *prog)
{
    uint64_t addr, len, total_size, total_len;
    uint32_t page, pages_in_block, page_size, block_size;
    np_scrub_cmd_t *scrub_cmd;

    if (prog->rx_buf_len < sizeof(np_scrub_cmd_t))
    {
        ERROR_PRINT("Wrong buffer length for scrub command %lu\r\n",
            prog->rx_buf_len);
        return NP_ERR_LEN_INVALID;
    }
    scrub_cmd = (np_scrub_cmd_t *)prog->rx_buf;
    total_len = len = scrub_cmd->len;
    addr = scrub_cmd->addr;

    pages_in_block = prog->chip_info.block_size / prog->chip_info.page_size;
    page_size = prog->chip_info.page_size;
    block_size = prog->chip_info.block_size;
    total_size = prog->chip_info.total_size;

    if (addr % block_size)
    {
        ERROR_PRINT("Scrub address 0x%llx" " is not aligned to block size 0x%lx\r\n", (unsigned long long)addr, block_size);
        return NP_ERR_ADDR_NOT_ALIGN;
    }

    if (!len || (len % block_size))
    {
        ERROR_PRINT("Scrub length 0x%llx" " is not aligned to block size 0x%lx\r\n", (unsigned long long)len, block_size);
        return NP_ERR_LEN_NOT_ALIGN;
    }

    if (addr + len > total_size)
    {
        ERROR_PRINT("Scrub address 0x%llx exceeds chip size 0x%llx" "\r\n", (unsigned long long)(addr + len), (unsigned long long)total_size);
        return NP_ERR_ADDR_EXCEEDED;
    }

    if (addr == 0 && len == total_size)
    {
        nand_bad_block_table_init();
        prog->bb_is_read = 0;
    }

    page = addr / page_size;

    while (len)
    {
        if (np_nand_erase(prog, page))
            return NP_ERR_NAND_ERASE;

        addr += block_size;
        page += pages_in_block;
        len -= block_size;

        np_send_progress(total_len - len);
    }

    return np_send_ok_status();
}

static int np_cmd_nand_scrub(np_prog_t *prog)
{
    int ret;
    led_wr_set(true);
    ret = _np_cmd_nand_scrub(prog);
    led_wr_set(false);
    return ret;
}

static inline uint32_t nand_prbs32_next(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void nand_generate_prbs_page(uint32_t page_num, uint32_t base_seed, uint8_t *buf, uint32_t data_size)
{
    uint32_t seed = (base_seed ^ (page_num * 0x1F1F1F1FU + 0x12345678U));
    if (seed == 0) seed = 0xA5A55A5AU;
    uint32_t *dst32 = (uint32_t *)buf;
    uint32_t words = data_size >> 2;
    while (words--) {
        *dst32++ = nand_prbs32_next(&seed);
    }
}

static void nand_calc_ecc_for_page(const uint8_t *data, uint32_t data_len, uint8_t *oob, uint32_t spare_len)
{
    memset(oob, 0xFF, spare_len);
    uint32_t sectors = data_len / 512;
    if (sectors == 0) sectors = 1;
    for (uint32_t s = 0; s < sectors; s++) {
        const uint8_t *sec_data = data + s * 512;
        uint8_t p_even = 0, p_odd = 0, col_parity = 0;
        for (int i = 0; i < 512; i++) {
            uint8_t b = sec_data[i];
            col_parity ^= b;
            if (__builtin_parity(b)) {
                p_odd ^= (uint8_t)i;
                p_even ^= (uint8_t)(~i);
            }
        }
        if (8 + s * 3 + 2 < spare_len) {
            oob[8 + s * 3 + 0] = col_parity;
            oob[8 + s * 3 + 1] = p_odd;
            oob[8 + s * 3 + 2] = p_even;
        }
    }
}

static int nand_verify_ecc_for_page(const uint8_t *data, uint32_t data_len, const uint8_t *oob, uint32_t spare_len)
{
    uint32_t sectors = data_len / 512;
    if (sectors == 0) sectors = 1;
    for (uint32_t s = 0; s < sectors; s++) {
        const uint8_t *sec_data = data + s * 512;
        uint8_t p_even = 0, p_odd = 0, col_parity = 0;
        for (int i = 0; i < 512; i++) {
            uint8_t b = sec_data[i];
            col_parity ^= b;
            if (__builtin_parity(b)) {
                p_odd ^= (uint8_t)i;
                p_even ^= (uint8_t)(~i);
            }
        }
        if (8 + s * 3 + 2 < spare_len) {
            if (oob[8 + s * 3 + 0] != col_parity ||
                oob[8 + s * 3 + 1] != p_odd ||
                oob[8 + s * 3 + 2] != p_even) {
                return -1;
            }
        }
    }
    return 0;
}

static void nand_re_mark_bad_block(np_prog_t *prog, uint32_t block_page)
{
    uint8_t zero_page[64];
    memset(zero_page, 0x00, sizeof(zero_page));
    hal[prog->hal]->write_page_async(zero_page, block_page, sizeof(zero_page));
    while (prog->nand_wr_in_progress) {
        np_nand_handle_status(prog);
    }
    hal[prog->hal]->write_page_async(zero_page, block_page + 1, sizeof(zero_page));
    while (prog->nand_wr_in_progress) {
        np_nand_handle_status(prog);
    }
    nand_bad_block_table_add(block_page);
}

static uint8_t test_read_buf[NP_MAX_PAGE_SIZE];
static uint8_t test_write_buf[NP_MAX_PAGE_SIZE];

static void nand_test_prepare_page(np_prog_t *prog, uint32_t page_num, uint32_t seed, uint8_t *out_raw, uint32_t data_page_size, uint32_t spare_size, qpic_ecc_mode_t qpic_ecc)
{
    nand_generate_prbs_page(page_num, seed, prog->page.buf, data_page_size);
    if (qpic_ecc != QPIC_ECC_NONE)
    {
        qpic_interleave_page(prog->page.buf, out_raw, data_page_size, spare_size, qpic_ecc);
    }
    else
    {
        memcpy(out_raw, prog->page.buf, data_page_size);
        nand_calc_ecc_for_page(out_raw, data_page_size, out_raw + data_page_size, spare_size);
    }
}

static bool nand_test_verify_page(np_prog_t *prog, uint32_t page_num, uint32_t seed, const uint8_t *read_raw, uint32_t data_page_size, uint32_t spare_size, qpic_ecc_mode_t qpic_ecc)
{
    nand_generate_prbs_page(page_num, seed, prog->page.buf, data_page_size);
    if (qpic_ecc != QPIC_ECC_NONE)
    {
        qpic_interleave_page(prog->page.buf, qpic_raw_buf, data_page_size, spare_size, qpic_ecc);
        return (memcmp(qpic_raw_buf, read_raw, data_page_size + spare_size) == 0);
    }
    else
    {
        if (memcmp(prog->page.buf, read_raw, data_page_size) != 0)
            return false;
        if (nand_verify_ecc_for_page(read_raw, data_page_size, read_raw + data_page_size, spare_size) != 0)
            return false;
        return true;
    }
}

static int _np_cmd_nand_test(np_prog_t *prog)
{
    uint64_t addr, len, total_size, total_len;
    uint32_t page, pages_in_block, data_page_size, spare_size, raw_page_size, block_size;
    np_test_cmd_t *test_cmd;
    uint8_t mode, base_mode, mark_bad;
    uint32_t seed;
    qpic_ecc_mode_t qpic_ecc;

    if (prog->rx_buf_len < sizeof(np_test_cmd_t))
    {
        ERROR_PRINT("Wrong buffer length for test command %lu\r\n", prog->rx_buf_len);
        return NP_ERR_LEN_INVALID;
    }
    test_cmd = (np_test_cmd_t *)prog->rx_buf;
    total_len = len = test_cmd->len;
    addr = test_cmd->addr;
    mode = test_cmd->mode;
    base_mode = mode & 0x0F;
    uint8_t qpic_opt = (mode >> 4) & 0x03;
    qpic_ecc = (qpic_opt == 2) ? QPIC_ECC_BCH8 : ((qpic_opt == 1) ? QPIC_ECC_BCH4 : QPIC_ECC_NONE);
    mark_bad = test_cmd->mark_bad;
    seed = test_cmd->seed ? test_cmd->seed : 0xA5A55A5AU;

    data_page_size = prog->chip_info.page_size;
    spare_size = prog->chip_info.spare_size;
    raw_page_size = data_page_size + spare_size;
    pages_in_block = prog->chip_info.block_size / data_page_size;
    block_size = prog->chip_info.block_size;
    total_size = prog->chip_info.total_size;

    if (addr % block_size)
    {
        ERROR_PRINT("Test address 0x%llx" " is not block aligned\r\n", (unsigned long long)addr);
        return NP_ERR_ADDR_NOT_ALIGN;
    }
    if (!len || (len % block_size))
    {
        ERROR_PRINT("Test length 0x%llx" " is not block aligned\r\n", (unsigned long long)len);
        return NP_ERR_LEN_NOT_ALIGN;
    }
    if (addr + len > total_size)
    {
        ERROR_PRINT("Test range exceeds chip size 0x%llx" "\r\n", (unsigned long long)total_size);
        return NP_ERR_ADDR_EXCEEDED;
    }

    if (base_mode == NP_TEST_MODE_FULL_CHIP)
    {
        /* Mode: Full-disk RDT spanning (Phase 1: Write all blocks -> Phase 2: Verify all blocks) */
        uint64_t cur_addr = addr;
        uint64_t rem_len = len;
        uint32_t cur_page = addr / data_page_size;

        /* Phase 1: Erase and write all blocks across the range */
        while (rem_len)
        {
            uint32_t block_start_page = cur_page;
            bool block_bad = false;

            if (np_nand_erase(prog, block_start_page))
            {
                block_bad = true;
                if (mark_bad)
                    nand_re_mark_bad_block(prog, block_start_page);
                np_send_bad_block_info(cur_addr, block_size, false);
            }

            if (!block_bad)
            {
                for (uint32_t p = 0; p < pages_in_block; p++)
                {
                    uint32_t page_num = block_start_page + p;
                    nand_test_prepare_page(prog, page_num, seed, test_write_buf, data_page_size, spare_size, qpic_ecc);

                    hal[prog->hal]->write_page_async(test_write_buf, page_num, raw_page_size);
                    prog->nand_wr_in_progress = 1;
                    while (prog->nand_wr_in_progress)
                    {
                        if (np_nand_handle_status(prog))
                        {
                            block_bad = true;
                            break;
                        }
                    }
                    if (block_bad)
                        break;
                }
                if (block_bad)
                {
                    if (mark_bad)
                        nand_re_mark_bad_block(prog, block_start_page);
                    np_send_bad_block_info(cur_addr, block_size, false);
                }
            }

            cur_addr += block_size;
            cur_page += pages_in_block;
            rem_len -= block_size;
            np_send_progress((total_len - rem_len) / 2);
        }

        /* Phase 2: Read back and verify all blocks across the range */
        cur_addr = addr;
        rem_len = len;
        cur_page = addr / data_page_size;

        while (rem_len)
        {
            uint32_t block_start_page = cur_page;
            bool block_bad = false;

            for (uint32_t p = 0; p < pages_in_block; p++)
            {
                uint32_t page_num = block_start_page + p;
                hal[prog->hal]->read_page(test_read_buf, page_num, raw_page_size);

                if (!nand_test_verify_page(prog, page_num, seed, test_read_buf, data_page_size, spare_size, qpic_ecc))
                {
                    block_bad = true;
                    break;
                }
            }
            if (block_bad)
            {
                if (mark_bad)
                    nand_re_mark_bad_block(prog, block_start_page);
                np_send_bad_block_info(cur_addr, block_size, false);
            }
            else
            {
                // Clean erase on success
                np_nand_erase(prog, block_start_page);
            }

            cur_addr += block_size;
            cur_page += pages_in_block;
            rem_len -= block_size;
            np_send_progress(total_len / 2 + (total_len - rem_len) / 2);
        }
    }
    else
    {
        /* Mode: Block-by-block, Write-only, or Verify-only */
        page = addr / data_page_size;

        while (len)
        {
            uint32_t block_start_page = page;
            bool block_bad = false;

            // Phase 1: Erase block if Full-Block or Write-Only mode
            if (base_mode == NP_TEST_MODE_FULL_BLOCK || base_mode == NP_TEST_MODE_WRITE_ONLY)
            {
                if (np_nand_erase(prog, block_start_page))
                {
                    block_bad = true;
                    if (mark_bad)
                        nand_re_mark_bad_block(prog, block_start_page);
                    np_send_bad_block_info(addr, block_size, false);
                }
            }

            // Phase 2: Write PRBS random data + ECC if Full-Block or Write-Only mode
            if (!block_bad && (base_mode == NP_TEST_MODE_FULL_BLOCK || base_mode == NP_TEST_MODE_WRITE_ONLY))
            {
                for (uint32_t p = 0; p < pages_in_block; p++)
                {
                    uint32_t cur_page = block_start_page + p;
                    nand_test_prepare_page(prog, cur_page, seed, test_write_buf, data_page_size, spare_size, qpic_ecc);

                    hal[prog->hal]->write_page_async(test_write_buf, cur_page, raw_page_size);
                    prog->nand_wr_in_progress = 1;
                    while (prog->nand_wr_in_progress)
                    {
                        if (np_nand_handle_status(prog))
                        {
                            block_bad = true;
                            break;
                        }
                    }
                    if (block_bad)
                        break;
                }
                if (block_bad)
                {
                    if (mark_bad)
                        nand_re_mark_bad_block(prog, block_start_page);
                    np_send_bad_block_info(addr, block_size, false);
                }
            }

            // Phase 3: Read back and verify PRBS + ECC if Full-Block or Verify-Only mode
            if (!block_bad && (base_mode == NP_TEST_MODE_FULL_BLOCK || base_mode == NP_TEST_MODE_VERIFY_ONLY))
            {
                for (uint32_t p = 0; p < pages_in_block; p++)
                {
                    uint32_t cur_page = block_start_page + p;
                    hal[prog->hal]->read_page(test_read_buf, cur_page, raw_page_size);

                    if (!nand_test_verify_page(prog, cur_page, seed, test_read_buf, data_page_size, spare_size, qpic_ecc))
                    {
                        block_bad = true;
                        break;
                    }
                }
                if (block_bad)
                {
                    if (mark_bad)
                        nand_re_mark_bad_block(prog, block_start_page);
                    np_send_bad_block_info(addr, block_size, false);
                }
                else if (base_mode == NP_TEST_MODE_FULL_BLOCK)
                {
                    // Clean erase on success in FULL_BLOCK mode
                    np_nand_erase(prog, block_start_page);
                }
            }

            addr += block_size;
            page += pages_in_block;
            len -= block_size;

            np_send_progress(total_len - len);
        }
    }

    return np_send_ok_status();
}

static int np_cmd_nand_test(np_prog_t *prog)
{
    int ret;
    led_wr_set(true);
    ret = _np_cmd_nand_test(prog);
    led_wr_set(false);
    return ret;
}

static int np_send_write_ack(uint64_t bytes_ack)
{
    np_resp_t resp_header = { NP_RESP_STATUS, NP_STATUS_WRITE_ACK };
    np_resp_write_ack_t write_ack = { resp_header, bytes_ack };

    if (np_comm_cb->send((uint8_t *)&write_ack, sizeof(write_ack)))
        return -1;

    return 0;
}

static int np_cmd_nand_write_start(np_prog_t *prog)
{
    int ret;
    uint64_t addr, len;
    uint32_t pages, pages_in_block;
    np_write_start_cmd_t *write_start_cmd;

    if (prog->rx_buf_len < sizeof(np_write_start_cmd_t))
    {
        ERROR_PRINT("Wrong buffer length for write start command %lu\r\n",
            prog->rx_buf_len);
        return NP_ERR_LEN_INVALID;
    }

    write_start_cmd = (np_write_start_cmd_t *)prog->rx_buf;

    if (hal[prog->hal]->enable_hw_ecc)
        hal[prog->hal]->enable_hw_ecc(write_start_cmd->flags.enable_hw_ecc);

    addr = write_start_cmd->addr;
    len = write_start_cmd->len;

    DEBUG_PRINT("Write at 0x%llx 0x%llx" " bytes command\r\n",
        (unsigned long long)addr, (unsigned long long)len);

    if (write_start_cmd->flags.qpic_bch8)
        prog->qpic_ecc = QPIC_ECC_BCH8;
    else if (write_start_cmd->flags.qpic_bch4)
        prog->qpic_ecc = QPIC_ECC_BCH4;
    else
        prog->qpic_ecc = QPIC_ECC_NONE;

    if (write_start_cmd->flags.inc_spare && prog->qpic_ecc == QPIC_ECC_NONE)
    {
        pages = prog->chip_info.total_size / prog->chip_info.page_size;
        pages_in_block = prog->chip_info.block_size /
            prog->chip_info.page_size;
        prog->page_size = prog->chip_info.page_size +
            prog->chip_info.spare_size;
        prog->block_size = pages_in_block * prog->page_size;
        prog->total_size = (uint64_t)pages * prog->page_size;
    }
    else
    {
        prog->page_size = prog->chip_info.page_size;
        prog->block_size = prog->chip_info.block_size;
        prog->total_size = prog->chip_info.total_size;
    }

    if (addr + len > prog->total_size)
    {
        ERROR_PRINT("Write address 0x%llx+0x%llx"
            " is more then chip size 0x%llx" "\r\n", (unsigned long long)addr, (unsigned long long)len,
            (unsigned long long)prog->total_size);
        return NP_ERR_ADDR_EXCEEDED;
    }

    if (addr % prog->page_size)
    {
        ERROR_PRINT("Address 0x%llx"
            " is not aligned to page size 0x%lx\r\n", (unsigned long long)addr, prog->page_size);
        return NP_ERR_ADDR_NOT_ALIGN;
    }

    if (!len)
    {
        ERROR_PRINT("Length is 0\r\n");
        return NP_ERR_LEN_INVALID;
    }

    if (len % prog->page_size)
    {
        ERROR_PRINT("Length 0x%llx"
            " is not aligned to page size 0x%lx\r\n", (unsigned long long)len, prog->page_size);
        return NP_ERR_LEN_NOT_ALIGN;
    }

    prog->skip_bb = write_start_cmd->flags.skip_bb;
    if (prog->skip_bb && !prog->bb_is_read &&
        (ret = _np_cmd_read_bad_blocks(prog, false)))
    {
        return ret;
    }

    if (prog->page_size > sizeof(prog->page.buf))
    {
        ERROR_PRINT("Page size 0x%lx"
            " is more then buffer size 0x%x\r\n", prog->page_size, sizeof(prog->page.buf));
        return NP_ERR_BUF_OVERFLOW;
    }

    prog->addr = addr;
    prog->len = len;
    prog->addr_is_set = 1;

    prog->page.page = addr / prog->page_size;
    prog->page.offset = 0;

    prog->bytes_written = 0;
    prog->bytes_ack = 0;

    return np_send_ok_status();
}

static int np_nand_handle_status(np_prog_t *prog)
{
    switch (hal[prog->hal]->read_status())
    {
    case FLASH_STATUS_ERROR:
        if (np_send_bad_block_info(prog->addr, prog->block_size, false))
            return -1;
        /* fall through */
    case FLASH_STATUS_READY:
        prog->nand_wr_in_progress = 0;
        prog->nand_timeout = 0;
        break;
    case FLASH_STATUS_BUSY:
        if (++prog->nand_timeout == NP_NAND_TIMEOUT)
        {
            ERROR_PRINT("NAND write timeout at 0x%llx" "\r\n", (unsigned long long)prog->addr);
            prog->nand_wr_in_progress = 0;
            prog->nand_timeout = 0;
            return -1;
        }
        break;
    default:
        ERROR_PRINT("Unknown NAND status\r\n");
        prog->nand_wr_in_progress = 0;
        prog->nand_timeout = 0;
        return -1;
    }

    return 0;
}

static int np_nand_write(np_prog_t *prog)
{   
    if (prog->nand_wr_in_progress)
    {
        DEBUG_PRINT("Wait for previous NAND write\r\n");
        do
        {
            if (np_nand_handle_status(prog))
                return -1;
        }
        while (prog->nand_wr_in_progress);
    }

    DEBUG_PRINT("NAND write at 0x%llx" " %lu bytes\r\n", (unsigned long long)prog->addr,
        prog->page_size);

    if (prog->qpic_ecc != QPIC_ECC_NONE)
    {
        uint32_t raw_page_size = prog->chip_info.page_size + prog->chip_info.spare_size;
        qpic_interleave_page(prog->page.buf, qpic_raw_buf, prog->chip_info.page_size, prog->chip_info.spare_size, (qpic_ecc_mode_t)prog->qpic_ecc);
        hal[prog->hal]->write_page_async(qpic_raw_buf, prog->page.page, raw_page_size);
    }
    else
    {
        hal[prog->hal]->write_page_async(prog->page.buf, prog->page.page,
            prog->page_size);
    }

    prog->nand_wr_in_progress = 1;

    return 0;
}

static int np_cmd_nand_write_data(np_prog_t *prog)
{
    uint32_t write_len, bytes_left, len;
    np_write_data_cmd_t *write_data_cmd;

    if (prog->rx_buf_len < sizeof(np_write_data_cmd_t))
    {
        ERROR_PRINT("Wrong buffer length for write data command %lu\r\n",
            prog->rx_buf_len);
        return NP_ERR_LEN_INVALID;
    }

    write_data_cmd = (np_write_data_cmd_t *)prog->rx_buf;
    len = write_data_cmd->len;
    if (len + sizeof(np_write_data_cmd_t) > NP_PACKET_BUF_SIZE)
    {
        ERROR_PRINT("Data size is wrong 0x%lx\r\n", len);
        return NP_ERR_CMD_DATA_SIZE;
    }

    if (len + sizeof(np_write_data_cmd_t) != prog->rx_buf_len)
    {
        ERROR_PRINT("Buffer len 0x%lx is bigger then command 0x%lx\r\n",
            prog->rx_buf_len, len + sizeof(np_write_data_cmd_t));
        return NP_ERR_CMD_DATA_SIZE;
    }

    if (!prog->addr_is_set)
    {
        ERROR_PRINT("Write address is not set\r\n");
        return NP_ERR_ADDR_INVALID;
    }

    if (prog->page.offset + len > prog->page_size)
        write_len = prog->page_size - prog->page.offset;
    else
        write_len = len;

    memcpy(prog->page.buf + prog->page.offset, write_data_cmd->data, write_len);
    prog->page.offset += write_len;

    if (prog->page.offset == prog->page_size)
    {
        while (prog->skip_bb && nand_bad_block_table_lookup(prog->page.page))
        {
            DEBUG_PRINT("Skipped bad block at 0x%llx" "\r\n", (unsigned long long)prog->addr);
            if (np_send_bad_block_info(prog->addr, prog->block_size, true))
                return -1;

            prog->addr += prog->block_size;
            prog->page.page += prog->block_size / prog->page_size;
        }

        if (prog->addr >= prog->total_size)
        {
            ERROR_PRINT("Write address 0x%llx"
                " is more then chip size 0x%llx" "\r\n", (unsigned long long)prog->addr,
                (unsigned long long)prog->total_size);
            return NP_ERR_ADDR_EXCEEDED;
        }

        if (np_nand_write(prog))
            return NP_ERR_NAND_WR;

        prog->addr += prog->page_size;
        prog->page.page++;
        prog->page.offset = 0;
    }

    bytes_left = len - write_len;
    if (bytes_left)
    {
        memcpy(prog->page.buf, write_data_cmd->data + write_len, bytes_left);
        prog->page.offset += bytes_left;
    }

    prog->bytes_written += len;
    if (prog->bytes_written - prog->bytes_ack >= prog->page_size ||
        prog->bytes_written == prog->len)
    {
        if (np_send_write_ack(prog->bytes_written))
            return -1;
        prog->bytes_ack = prog->bytes_written;
    }

    if (prog->bytes_written > prog->len)
    {
        ERROR_PRINT("Actual write data length 0x%llx"
            " is more then 0x%llx" "\r\n", (unsigned long long)prog->bytes_written, (unsigned long long)prog->len);
        return NP_ERR_LEN_EXCEEDED;
    }

    return 0;
}

static int np_cmd_nand_write_end(np_prog_t *prog)
{
    prog->addr_is_set = 0;

    if (prog->page.offset)
    {
        ERROR_PRINT("Data of 0x%lx length was not written\r\n",
            prog->page.offset);
        return NP_ERR_NAND_WR;
    }

    while (prog->nand_wr_in_progress)
    {
        if (np_nand_handle_status(prog))
            return NP_ERR_NAND_WR;
    }

    return np_send_ok_status();
}

static int np_cmd_nand_write(np_prog_t *prog)
{
    np_cmd_t *cmd = (np_cmd_t *)prog->rx_buf;
    int ret = 0;

    switch (cmd->code)
    {
    case NP_CMD_NAND_WRITE_S:
        led_wr_set(true);
        ret = np_cmd_nand_write_start(prog);
        break;
    case NP_CMD_NAND_WRITE_D:
        ret = np_cmd_nand_write_data(prog);
        break;
    case NP_CMD_NAND_WRITE_E:
        ret = np_cmd_nand_write_end(prog);
        led_wr_set(false);
        break;
    default:
        break;
    }

    if (ret < 0)
        led_wr_set(false);

    return ret;
}

static int np_nand_read(uint64_t addr, np_page_t *page, uint32_t page_size,
    uint32_t block_size, np_prog_t *prog)
{
    uint32_t status;

    status = hal[prog->hal]->read_page(page->buf, page->page, page_size);
    switch (status)
    {
    case FLASH_STATUS_READY:
        break;
    case FLASH_STATUS_ERROR:
        if (np_send_bad_block_info(addr, block_size, false))
            return -1;
        break;
    case FLASH_STATUS_TIMEOUT:
        ERROR_PRINT("NAND write timeout at 0x%llx" "\r\n", (unsigned long long)addr);
        break;
    default:
        ERROR_PRINT("Unknown NAND status\r\n");
        return -1;
    }

    return 0;
}

static int _np_cmd_nand_read(np_prog_t *prog)
{
    int ret;
    static np_page_t page;
    np_read_cmd_t *read_cmd;
    bool skip_bb, inc_spare;
    uint64_t addr, len, total_size;
    uint32_t send_len, block_size, page_size, pages,
        pages_in_block;
    uint32_t resp_header_size = offsetof(np_resp_t, data);
    uint32_t tx_data_len = sizeof(np_packet_send_buf) - resp_header_size;
    np_resp_t *resp = (np_resp_t *)np_packet_send_buf;

    if (prog->rx_buf_len < sizeof(np_read_cmd_t))
    {
        ERROR_PRINT("Wrong buffer length for read command %lu\r\n",
            prog->rx_buf_len);
        return NP_ERR_LEN_INVALID;
    }

    read_cmd = (np_read_cmd_t *)prog->rx_buf;
    addr = read_cmd->addr;
    len = read_cmd->len;
    skip_bb = read_cmd->flags.skip_bb;
    inc_spare = read_cmd->flags.inc_spare;

    qpic_ecc_mode_t qpic_ecc = QPIC_ECC_NONE;
    if (read_cmd->flags.qpic_bch8)
        qpic_ecc = QPIC_ECC_BCH8;
    else if (read_cmd->flags.qpic_bch4)
        qpic_ecc = QPIC_ECC_BCH4;

    DEBUG_PRINT("Read at 0x%llx 0x%llx" " bytes command\r\n", (unsigned long long)addr,
        len);

    if (inc_spare && qpic_ecc == QPIC_ECC_NONE)
    {
        pages = prog->chip_info.total_size / prog->chip_info.page_size;
        pages_in_block = prog->chip_info.block_size /
            prog->chip_info.page_size;
        page_size = prog->chip_info.page_size + prog->chip_info.spare_size;
        block_size = pages_in_block * page_size;
        total_size = (uint64_t)pages * page_size;
    }
    else
    {
        page_size = prog->chip_info.page_size;
        block_size = prog->chip_info.block_size;
        total_size = prog->chip_info.total_size;
    }

    if (addr + len > total_size)
    {
        ERROR_PRINT("Read address 0x%llx+0x%llx"
            " is more then chip size 0x%llx" "\r\n", (unsigned long long)addr, (unsigned long long)len, (unsigned long long)total_size);
        return NP_ERR_ADDR_EXCEEDED;
    }

    if (addr % page_size)
    {
        ERROR_PRINT("Read address 0x%llx"
            " is not aligned to page size 0x%lx\r\n", (unsigned long long)addr, page_size);
        return NP_ERR_ADDR_NOT_ALIGN;
    }

    if (!len)
    {
        ERROR_PRINT("Length is 0\r\n");
        return NP_ERR_LEN_INVALID;
    }

    if (len % page_size)
    {
        ERROR_PRINT("Read length 0x%llx"
            " is not aligned to page size 0x%lx\r\n", (unsigned long long)len, page_size);
        return NP_ERR_LEN_NOT_ALIGN;
    }

    if (skip_bb && !prog->bb_is_read && (ret = _np_cmd_read_bad_blocks(prog,
        false)))
    {
        return ret;
    }

    page.page = addr / page_size;
    page.offset = 0;

    resp->code = NP_RESP_DATA;

    while (len)
    {
        if (addr >= total_size)
        {
            ERROR_PRINT("Read address 0x%llx"
                " is more then chip size 0x%llx" "\r\n", (unsigned long long)addr, (unsigned long long)total_size);
            return NP_ERR_ADDR_EXCEEDED;
        }

        if (skip_bb && nand_bad_block_table_lookup(page.page))
        {
            DEBUG_PRINT("Skipped bad block at 0x%llx" "\r\n", (unsigned long long)addr);
            if (np_send_bad_block_info(addr, block_size, true))
                return -1;

            /* On partial read do not count bad blocks */
            if (read_cmd->len == total_size)
                len -= block_size;
            addr += block_size;
            page.page += block_size / page_size;
            continue;
        }

        if (qpic_ecc != QPIC_ECC_NONE)
        {
            uint32_t raw_page_size = prog->chip_info.page_size + prog->chip_info.spare_size;
            uint32_t status = hal[prog->hal]->read_page(qpic_raw_buf, page.page, raw_page_size);
            if (status == FLASH_STATUS_ERROR)
            {
                if (np_send_bad_block_info(addr, block_size, false))
                    return -1;
            }
            else if (status != FLASH_STATUS_READY)
            {
                ERROR_PRINT("NAND read timeout or error at 0x%llx\r\n", (unsigned long long)addr);
                return NP_ERR_NAND_RD;
            }
            qpic_deinterleave_page(qpic_raw_buf, page.buf, prog->chip_info.page_size, prog->chip_info.spare_size, qpic_ecc);
        }
        else
        {
            if (np_nand_read(addr, &page, page_size, block_size, prog))
                return NP_ERR_NAND_RD;
        }

        while (page.offset < page_size && len)
        {
            if (page_size - page.offset >= tx_data_len)
                send_len = tx_data_len;
            else
                send_len = page_size - page.offset;

            if (send_len > len)
                send_len = len;

            memcpy(resp->data, page.buf + page.offset, send_len);

            while (!np_comm_cb->send_ready());

            resp->info = send_len;
            if (np_comm_cb->send(np_packet_send_buf,
                resp_header_size + send_len))
            {
                return -1;
            }

            page.offset += send_len;
            len -= send_len;
        }

        addr += page_size;
        page.offset = 0;
        page.page++;
    }

    return 0;
}

static int np_cmd_nand_read(np_prog_t *prog)
{
    int ret;

    led_rd_set(true);
    ret = _np_cmd_nand_read(prog);
    led_rd_set(false);

    return ret;
}

static void np_fill_chip_info(np_conf_cmd_t *conf_cmd, np_prog_t *prog)
{
    prog->chip_info.page_size = conf_cmd->page_size;
    prog->chip_info.block_size = conf_cmd->block_size;
    prog->chip_info.total_size = conf_cmd->total_size;
    prog->chip_info.spare_size = conf_cmd->spare_size;
    prog->chip_info.bb_mark_off = conf_cmd->bb_mark_off;
    prog->chip_is_conf = 1;
}

static void np_print_chip_info(np_prog_t *prog)
{
    DEBUG_PRINT("Page size: %lu\r\n", prog->chip_info.page_size);
    DEBUG_PRINT("Block size: %lu\r\n", prog->chip_info.block_size);
    DEBUG_PRINT("Total size: 0x%llx" "\r\n", (unsigned long long)prog->chip_info.total_size);
    DEBUG_PRINT("Spare size: %lu\r\n", prog->chip_info.spare_size);    
    DEBUG_PRINT("Bad block mark offset: %d\r\n", prog->chip_info.bb_mark_off);
}

static int np_cmd_nand_conf(np_prog_t *prog)
{
    np_conf_cmd_t *conf_cmd;

    DEBUG_PRINT("Chip configure command\r\n");

    if (prog->rx_buf_len < sizeof(np_conf_cmd_t))
    {
        ERROR_PRINT("Wrong buffer length for configuration command %lu\r\n",
            prog->rx_buf_len);
        return NP_ERR_LEN_INVALID;
    }

    conf_cmd = (np_conf_cmd_t *)prog->rx_buf;

    np_fill_chip_info(conf_cmd, prog);
    np_print_chip_info(prog);

    prog->hal = conf_cmd->hal;
    if (hal[prog->hal]->init(conf_cmd->hal_conf,
        prog->rx_buf_len - sizeof(np_conf_cmd_t)))
    {
        ERROR_PRINT("Wrong buffer length for hal configuration command %lu\r\n",
            prog->rx_buf_len);
        return NP_ERR_LEN_INVALID;
    }

    nand_bad_block_table_init();
    prog->bb_is_read = 0;

    return np_send_ok_status();
}

static int np_send_bad_blocks(np_prog_t *prog)
{
    uint32_t page;
    void *bb_iter;

    for (bb_iter = nand_bad_block_table_iter_alloc(&page); bb_iter;
        bb_iter = nand_bad_block_table_iter_next(bb_iter, &page))
    {
        if (np_send_bad_block_info(page * prog->chip_info.page_size,
            prog->chip_info.block_size, false))
        {
            return -1;
        }
    }

    return 0;
}

int np_cmd_read_bad_blocks(np_prog_t *prog)
{
    int ret;

    led_rd_set(true);
    nand_bad_block_table_init();  
    ret = _np_cmd_read_bad_blocks(prog, true);
    led_rd_set(false);

    if (ret || (ret = np_send_bad_blocks(prog)))
        return ret;

    return np_send_ok_status();
}

int np_cmd_version_get(np_prog_t *prog)
{
    np_resp_version_t resp;
    size_t resp_len = sizeof(resp);

    DEBUG_PRINT("Read version command\r\n");

    resp.header.code = NP_RESP_DATA;
    resp.header.info = resp_len - sizeof(resp.header);
    resp.version.major = SW_VERSION_MAJOR;
    resp.version.minor = SW_VERSION_MINOR;
    resp.version.build = SW_VERSION_BUILD;

    if (np_comm_cb)
        np_comm_cb->send((uint8_t *)&resp, resp_len);

    return 0;
}

static int np_boot_config_read(boot_config_t *config)
{
    if (flash_read(BOOT_CONFIG_ADDR, (uint8_t *)config, sizeof(boot_config_t))
        < 0)
    {
        return -1;
    }
    
    return 0;
}

static int np_boot_config_write(boot_config_t *config)
{
    if (flash_page_erase(BOOT_CONFIG_ADDR) < 0)
        return -1;

    if (flash_write(BOOT_CONFIG_ADDR, (uint8_t *)config, sizeof(boot_config_t))
        < 0)
    {
        return -1;
    }

    return 0;
}

static int np_cmd_active_image_get(np_prog_t *prog)
{
    boot_config_t boot_config;
    np_resp_active_image_t resp;
    size_t resp_len = sizeof(resp);

    DEBUG_PRINT("Get active image command\r\n");

    if (prog->active_image == 0xff)
    {
        if (np_boot_config_read(&boot_config))
            return NP_ERR_INTERNAL;
        prog->active_image = boot_config.active_image;
    }

    resp.header.code = NP_RESP_DATA;
    resp.header.info = resp_len - sizeof(resp.header);
    resp.active_image = prog->active_image;

    if (np_comm_cb)
        np_comm_cb->send((uint8_t *)&resp, resp_len);

    return 0;
}

static int np_cmd_fw_update_start(np_prog_t *prog)
{
    uint64_t addr, len;
    np_write_start_cmd_t *write_start_cmd;

    if (prog->rx_buf_len < sizeof(np_write_start_cmd_t))
    {
        ERROR_PRINT("Wrong buffer length for write start command %lu\r\n",
            prog->rx_buf_len);
        return NP_ERR_LEN_INVALID;
    }

    write_start_cmd = (np_write_start_cmd_t *)prog->rx_buf;
    addr = write_start_cmd->addr;
    len = write_start_cmd->len;

    DEBUG_PRINT("Write at 0x%llx 0x%llx" " bytes command\r\n", (unsigned long long)addr,
        len);

    prog->base_addr = FLASH_START_ADDR;
    prog->page_size = FLASH_PAGE_SIZE;
    prog->block_size = FLASH_BLOCK_SIZE;
    prog->total_size = FLASH_SIZE;

    if (addr + len > prog->base_addr + prog->total_size)
    {
        ERROR_PRINT("Write address 0x%llx+0x%llx"
            " is more then flash size 0x%llx" "\r\n", (unsigned long long)addr, (unsigned long long)len,
            (unsigned long long)(prog->base_addr + prog->total_size));
        return NP_ERR_ADDR_EXCEEDED;
    }

    if (addr % prog->page_size)
    {
        ERROR_PRINT("Address 0x%llx"
            " is not aligned to page size 0x%lx\r\n", (unsigned long long)addr, prog->page_size);
        return NP_ERR_ADDR_NOT_ALIGN;
    }

    if (!len)
    {
        ERROR_PRINT("Length is 0\r\n");
        return NP_ERR_LEN_INVALID;
    }

    if (len % prog->page_size)
    {
        ERROR_PRINT("Length 0x%llx"
            " is not aligned to page size 0x%lx\r\n", (unsigned long long)len, prog->page_size);
        return NP_ERR_LEN_NOT_ALIGN;
    }

    prog->addr = addr;
    prog->len = len;
    prog->addr_is_set = 1;

    prog->page.page = addr / prog->page_size;
    prog->page.offset = 0;

    prog->bytes_written = 0;
    prog->bytes_ack = 0;

    return np_send_ok_status();
}

static int np_cmd_fw_update_data(np_prog_t *prog)
{
    uint32_t write_len;
    uint64_t bytes_left, len;
    np_write_data_cmd_t *write_data_cmd;

    if (prog->rx_buf_len < sizeof(np_write_data_cmd_t))
    {
        ERROR_PRINT("Wrong buffer length for write data command %lu\r\n",
            prog->rx_buf_len);
        return NP_ERR_LEN_INVALID;
    }

    write_data_cmd = (np_write_data_cmd_t *)prog->rx_buf;
    len = write_data_cmd->len;
    if (len + sizeof(np_write_data_cmd_t) > NP_PACKET_BUF_SIZE)
    {
        ERROR_PRINT("Data size is wrong 0x%llx" "\r\n", (unsigned long long)len);
        return NP_ERR_CMD_DATA_SIZE;
    }

    if (len + sizeof(np_write_data_cmd_t) != prog->rx_buf_len)
    {
        ERROR_PRINT("Buffer len 0x%lx is bigger then command 0x%lx\r\n",
            prog->rx_buf_len, (unsigned long)len + sizeof(np_write_data_cmd_t));
        return NP_ERR_CMD_DATA_SIZE;
    }

    if (!prog->addr_is_set)
    {
        ERROR_PRINT("Write address is not set\r\n");
        return NP_ERR_ADDR_INVALID;
    }

    if (prog->page.offset + len > prog->page_size)
        write_len = prog->page_size - prog->page.offset;
    else
        write_len = len;

    memcpy(prog->page.buf + prog->page.offset, write_data_cmd->data, write_len);
    prog->page.offset += write_len;

    if (prog->page.offset == prog->page_size)
    {
        if (prog->addr >= prog->base_addr + prog->total_size)
        {
            ERROR_PRINT("Write address 0x%llx"
                " is more then flash size 0x%llx" "\r\n",
                (unsigned long long)prog->addr, (unsigned long long)(prog->base_addr + prog->total_size));
            return NP_ERR_ADDR_EXCEEDED;
        }

        if (flash_page_erase((uint32_t)prog->addr) < 0)
            return NP_ERR_INTERNAL;

        if (flash_write((uint32_t)prog->addr, prog->page.buf,
            prog->page_size) < 0)
        {
            return NP_ERR_INTERNAL;
        }

        prog->addr += prog->page_size;
        prog->page.page++;
        prog->page.offset = 0;
    }

    bytes_left = len - write_len;
    if (bytes_left)
    {
        memcpy(prog->page.buf, write_data_cmd->data + write_len, bytes_left);
        prog->page.offset += bytes_left;
    }

    prog->bytes_written += len;
    if (prog->bytes_written - prog->bytes_ack >= prog->page_size ||
        prog->bytes_written == prog->len)
    {
        if (np_send_write_ack(prog->bytes_written))
            return -1;
        prog->bytes_ack = prog->bytes_written;
    }

    if (prog->bytes_written > prog->len)
    {
        ERROR_PRINT("Actual write data length 0x%llx"
            " is more then 0x%llx" "\r\n", (unsigned long long)prog->bytes_written, (unsigned long long)prog->len);
        return NP_ERR_LEN_EXCEEDED;
    }

    return 0;
}

static int np_cmd_fw_update_end(np_prog_t *prog)
{
    boot_config_t boot_config;

    prog->addr_is_set = 0;

    if (prog->page.offset)
    {
        ERROR_PRINT("Data of 0x%lx length was not written\r\n",
            prog->page.offset);
        return NP_ERR_NAND_WR;
    }

    if (np_boot_config_read(&boot_config))
        return NP_ERR_INTERNAL;

    if (prog->active_image == 0xff)
        prog->active_image = boot_config.active_image;
    boot_config.active_image = prog->active_image ? 0 : 1;
    if (np_boot_config_write(&boot_config))
        return NP_ERR_INTERNAL;

    return np_send_ok_status();
}

static int np_cmd_fw_update(np_prog_t *prog)
{
    np_cmd_t *cmd = (np_cmd_t *)prog->rx_buf;
    int ret = 0;

    switch (cmd->code)
    {
    case NP_CMD_FW_UPDATE_S:
        led_wr_set(true);
        ret = np_cmd_fw_update_start(prog);
        break;
    case NP_CMD_FW_UPDATE_D:
        ret = np_cmd_fw_update_data(prog);
        break;
    case NP_CMD_FW_UPDATE_E:
        ret = np_cmd_fw_update_end(prog);
        led_wr_set(false);
        break;
    default:
        break;
    }

    if (ret < 0)
        led_wr_set(false);

    return ret;
}

static int np_cmd_nand_probe_onfi(np_prog_t *prog)
{
    onfi_param_page_t onfi;
    if (fsmc_nand_read_onfi(&onfi) != 0)
    {
        return NP_ERR_CHIP_NOT_CONF;
    }

    prog->chip_info.page_size = onfi.page_data_bytes;
    prog->chip_info.spare_size = onfi.page_spare_bytes;
    prog->chip_info.block_size = onfi.pages_per_block * onfi.page_data_bytes;
    prog->chip_info.total_size = (uint64_t)onfi.blocks_per_lun * onfi.lun_count * prog->chip_info.block_size;
    prog->chip_info.bb_mark_off = 0;

    prog->page_size = prog->chip_info.page_size;
    prog->block_size = prog->chip_info.block_size;
    prog->total_size = prog->chip_info.total_size;

    fsmc_conf_t auto_conf;
    memset(&auto_conf, 0, sizeof(auto_conf));
    auto_conf.setup_time = 20;
    auto_conf.wait_setup_time = 12;
    auto_conf.hold_setup_time = 12;
    auto_conf.hi_z_setup_time = 10;
    auto_conf.clr_setup_time = 10;
    auto_conf.ar_setup_time = 12;
    auto_conf.row_cycles = onfi.address_cycles & 0x0F;
    auto_conf.col_cycles = (onfi.address_cycles >> 4) & 0x0F;
    auto_conf.read1_cmd = 0x00;
    auto_conf.read2_cmd = 0x30;
    auto_conf.read_spare_cmd = 0xFF;
    auto_conf.read_id_cmd = 0x90;
    auto_conf.reset_cmd = 0xFF;
    auto_conf.write1_cmd = 0x80;
    auto_conf.write2_cmd = 0x10;
    auto_conf.erase1_cmd = 0x60;
    auto_conf.erase2_cmd = 0xD0;
    auto_conf.status_cmd = 0x70;
    auto_conf.set_features_cmd = 0xEF;
    auto_conf.enable_ecc_addr = 0x90;
    auto_conf.enable_ecc_value = 0x08;
    auto_conf.disable_ecc_value = 0x00;

    hal[0]->init(&auto_conf, sizeof(auto_conf));
    prog->hal = 0;
    prog->chip_is_conf = 1;
    prog->bb_is_read = 0;
    nand_bad_block_table_init();

    np_resp_onfi_t resp;
    resp.header.code = NP_RESP_DATA;
    resp.header.info = sizeof(resp) - sizeof(resp.header);
    memcpy(resp.manufacturer, onfi.manufacturer, 12);
    memcpy(resp.model, onfi.model, 20);
    resp.page_size = prog->chip_info.page_size;
    resp.block_size = prog->chip_info.block_size;
    resp.total_size = prog->chip_info.total_size;
    resp.spare_size = prog->chip_info.spare_size;
    resp.row_cycles = auto_conf.row_cycles;
    resp.col_cycles = auto_conf.col_cycles;

    return np_comm_cb->send((uint8_t *)&resp, sizeof(resp));
}

static np_cmd_handler_t cmd_handler[] =
{
    { NP_CMD_NAND_READ_ID, 1, np_cmd_nand_read_id },
    { NP_CMD_NAND_ERASE, 1, np_cmd_nand_erase },
    { NP_CMD_NAND_READ, 1, np_cmd_nand_read },
    { NP_CMD_NAND_WRITE_S, 1, np_cmd_nand_write },
    { NP_CMD_NAND_WRITE_D, 1, np_cmd_nand_write },
    { NP_CMD_NAND_WRITE_E, 1, np_cmd_nand_write },
    { NP_CMD_NAND_CONF, 0, np_cmd_nand_conf },
    { NP_CMD_NAND_READ_BB, 1, np_cmd_read_bad_blocks },
    { NP_CMD_VERSION_GET, 0, np_cmd_version_get },
    { NP_CMD_ACTIVE_IMAGE_GET, 0, np_cmd_active_image_get },
    { NP_CMD_FW_UPDATE_S, 0, np_cmd_fw_update },
    { NP_CMD_FW_UPDATE_D, 0, np_cmd_fw_update },
    { NP_CMD_FW_UPDATE_E, 0, np_cmd_fw_update },
    { 0x0d, 0, NULL },
    { 0x0e, 0, NULL },
    { 0x0f, 0, NULL },
    { NP_CMD_NAND_SCRUB, 1, np_cmd_nand_scrub },
    { NP_CMD_NAND_TEST, 1, np_cmd_nand_test },
    { NP_CMD_NAND_PROBE_ONFI, 0, np_cmd_nand_probe_onfi },
};

static bool np_cmd_is_valid(np_cmd_code_t code)
{
    return code >= 0 && code < NP_CMD_NAND_LAST;
}

static int np_cmd_handler(np_prog_t *prog)
{
    np_cmd_t *cmd;

    if (prog->rx_buf_len < sizeof(np_cmd_t))
    {
        ERROR_PRINT("Wrong buffer length for command %lu\r\n",
            prog->rx_buf_len);
        return NP_ERR_LEN_INVALID;
    }
    cmd = (np_cmd_t *)prog->rx_buf;

    if (!np_cmd_is_valid(cmd->code))
    {
        ERROR_PRINT("Invalid cmd code %d\r\n", cmd->code);
        return NP_ERR_CMD_INVALID;
    }

    if (!prog->chip_is_conf && cmd_handler[cmd->code].is_chip_cmd)
    {
        ERROR_PRINT("Chip is not configured\r\n");
        return NP_ERR_CHIP_NOT_CONF;
    }

    if (!cmd_handler[cmd->code].exec)
    {
        ERROR_PRINT("Command %d has no handler\r\n", cmd->code);
        return NP_ERR_CMD_INVALID;
    }

    return cmd_handler[cmd->code].exec(prog);
}

static void np_packet_handler(np_prog_t *prog)
{
    int ret;

    do
    {
        prog->rx_buf_len = np_comm_cb->peek(&prog->rx_buf);
        if (!prog->rx_buf_len)
            break;

        ret = np_cmd_handler(prog);

        np_comm_cb->consume();

        if (ret < 0)
            np_send_error(-ret);
    }
    while (1);
}

static void np_nand_handler(np_prog_t *prog)
{
    if (prog->nand_wr_in_progress)
    {
        if (np_nand_handle_status(prog))
            np_send_error(NP_ERR_NAND_WR);
    }
}

void np_init()
{
    prog.active_image = 0xff;
    qpic_init();
}

void np_handler()
{
    np_packet_handler(&prog);
    np_nand_handler(&prog);
}

int np_comm_register(np_comm_cb_t *cb)
{
    np_comm_cb = cb;

    return 0;
}

void np_comm_unregister(np_comm_cb_t *cb)
{
    if (np_comm_cb == cb)
        np_comm_cb = NULL;
}

