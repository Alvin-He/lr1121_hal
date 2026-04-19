// Raspberry-PI or generic linux implenmentation based on 
// SWSD003's HAL for STM32: https://github.com/Lora-net/SWSD003/blob/08912a2324bfc931224d368984b58b4a853078ad/lr11xx/common/lr11xx_hal.c

#ifndef LR11XX_LINUX_HAL_C
#define LR11XX_LINUX_HAL_C


#include "lr11xx_hal.h"
#include "lr11xx_types.h"
#include "lr11xx_linux_hal.h"
#include <bits/types.h>
#include <errno.h>
#include <linux/spi/spi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <lgpio.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <fcntl.h>

#include <linux/spi/spidev.h>

#define checkbs(s) if(!(s)) return false;
#define checkls(s) if(!(s)) return LR11XX_HAL_STATUS_ERROR;

lr11xx_hal_context_t* lr11xx_init_hal(
    const char* spi_device_path, const int gpio_chip_number,
    const int reset_pin, const int nss_pin, const int busy_pin
) {
    assert(spi_device_path); 

    // alloc context & mem structs 
    lr11xx_hal_context_t* ctx = malloc(sizeof(lr11xx_hal_context_t)); assert(ctx);

    ///////////
    /// SPI ///
    ///////////
    int dev = ctx->spi_device = openat(AT_FDCWD, spi_device_path, O_RDWR | O_SYNC); assert(dev >= 0);
    
    // set spi mode
    const uint8_t mode = LR1121_KSPI_MODE;
    assert(ioctl(dev, SPI_IOC_WR_MODE, &mode) >= 0);
    uint8_t mode_set;
    assert(ioctl(dev, SPI_IOC_RD_MODE, &mode_set) >= 0);
    assert(mode_set == mode);

    // set spi speed
    const uint32_t speed = LR1121_KSPI_MAX_SPEED_HZ;
    assert(ioctl(dev, SPI_IOC_WR_MAX_SPEED_HZ, &speed) >= 0);
    uint32_t speed_set;
    assert(ioctl(dev, SPI_IOC_RD_MAX_SPEED_HZ, &speed_set) >= 0);
    assert(speed_set == speed);

    // set spi bits per word
    const uint8_t bpw = LR1121_KBITS_PER_WORD;
    assert(ioctl(dev, SPI_IOC_WR_BITS_PER_WORD, &bpw) >= 0); // 0 is 8 bits per word
    uint8_t bpw_set; 
    assert(ioctl(dev, SPI_IOC_RD_BITS_PER_WORD, &bpw_set) >= 0);
    assert(bpw_set == bpw || (bpw_set == 8 && bpw == 0));

    // set spi bit order
    uint8_t bit_order = LR1121_KSPI_IS_LSB_FIRST;
    assert(ioctl(dev, SPI_IOC_WR_LSB_FIRST, &bit_order) >= 0);
    uint8_t bit_order_set;
    assert(ioctl(dev, SPI_IOC_RD_LSB_FIRST, &bit_order_set) >= 0);
    assert(bit_order_set == bit_order);

    ////////////
    /// GPIO ///
    ////////////

    // get GPIO chip
    // struct gpiod_chip* chip = mem->chip = gpiod_chip_open(gpio_device_path); assert(chip); 
    int gpio_dev = lgGpiochipOpen(gpio_chip_number); assert(gpio_dev >= 0);
    ctx->gpio_device = gpio_dev;

    ctx->busy_pin = busy_pin;
    ctx->nss_pin = nss_pin;
    ctx->reset_pin = reset_pin;
    
    // busy pin: floating
    assert(lgGpioClaimInput(gpio_dev, LG_SET_PULL_NONE, busy_pin) == 0);
    // reset pin: push pull, initially high
    assert(lgGpioClaimOutput(gpio_dev, 0, reset_pin, 1) == 0);
    // nss pin: push pull, initially high
    // assert(lgGpioClaimOutput(gpio_dev, 0, nss_pin, 1) == 0);

    return ctx;
}

void lr11xx_close_hal(lr11xx_hal_context_t* ctx) {
    // relase spi
    close(ctx->spi_device); ctx->spi_device = -1;

    lgGpioFree(ctx->gpio_device, ctx->busy_pin);
    lgGpioFree(ctx->gpio_device, ctx->reset_pin);
    // lgGpioFree(ctx->gpio_device, ctx->nss_pin);
    lgGpiochipClose(ctx->gpio_device);

    // free the ctx and mem structs
    free(ctx); ctx = NULL;
}

/**
 * @brief Wait until radio busy pin returns to 0
 */
bool lr11xx_hal_wait_while_busy(const lr11xx_hal_context_t* ctx) {
    int state = 1;
    do {
        state = lgGpioRead(ctx->gpio_device, ctx->busy_pin);
        checkbs(state >= 0);
        usleep(100);
    }while (state == 1);
    return true;
} 

/**
 * @brief Send an IOC transfer to spi bus. Along with handling NSS & BUSY signals 
 */
bool lr11xx_hal_send_ioc_transfer(const lr11xx_hal_context_t* ctx, struct spi_ioc_transfer* transfer) {
    checkbs(lr11xx_hal_wait_while_busy(ctx));

    if (transfer->tx_buf != 0) {
        printf("Written: ");
        uint8_t* ptr = (uint8_t*)transfer->tx_buf;
        for (uint32_t i = 0; i < transfer->len; i++) {
            printf("%02hhx ", ptr[i]);
        }
        printf("\n");
    }

    uint8_t* debug_rx_buf = NULL;
    if (transfer->rx_buf == 0) {
        debug_rx_buf = calloc(1, transfer->len); assert(debug_rx_buf);
        transfer->rx_buf = (__u64)debug_rx_buf;
    }

    // checkls(lgGpioWrite(ctx->gpio_device, ctx->nss_pin, 0) == 0);

    checkbs(ioctl(ctx->spi_device, SPI_IOC_MESSAGE(1), transfer) >= 0);

    // checkls(lgGpioWrite(ctx->gpio_device, ctx->nss_pin, 1) == 0)

    if (transfer->rx_buf != 0) {        
        printf("Read:    ");
        uint8_t* ptr = (uint8_t*)transfer->rx_buf;
        for (uint32_t i = 0; i < transfer->len; i++) {
            printf("%02hhx ", ptr[i]);
        }
        printf("\n");
    }

    if (debug_rx_buf) {
        free(debug_rx_buf);
    }

    return true;
}

/**
 * @brief Coverts a const void* into a const lr11xx_hal_context_t*
 */
const lr11xx_hal_context_t* lr11xx_hal_context_from_ptr(const void* ctx) {
    assert(ctx);
    return (const lr11xx_hal_context_t*) ctx;
}

lr11xx_hal_status_t lr11xx_boostrap(lr11xx_hal_context_t* ctx) {
    printf("BOOTSTRAP=system_reset\n");
    // free busy pin
    checkls(lgGpioFree(ctx->gpio_device, ctx->busy_pin) == 0);

    // reclaim busy an output, and pull down & write low
    checkls(lgGpioClaimOutput(ctx->gpio_device, LG_SET_PULL_DOWN, ctx->busy_pin, 0) == 0);
    checkls(lgGpioWrite(ctx->gpio_device, ctx->busy_pin, 0) == 0);

    // issue a reset
    checkls(lr11xx_hal_reset(ctx) == LR11XX_HAL_STATUS_OK);

    // continue holding busy
    usleep(500*1000); // sleep for 500ms

    // reset busy back to input
    checkls(lgGpioFree( ctx->gpio_device, ctx->busy_pin) == 0);
    checkls(lgGpioClaimInput(ctx->gpio_device, LG_SET_PULL_NONE, ctx->busy_pin) == 0);

    usleep(100*1000); // hold for 100ms more
    return LR11XX_HAL_STATUS_OK;
}

lr11xx_hal_status_t lr11xx_hal_read_write(const void *context, const uint8_t *cmd_write, uint8_t *data_read, const uint16_t length) {
    const lr11xx_hal_context_t* ctx = lr11xx_hal_context_from_ptr(context);

    //  ioc transfer
    struct spi_ioc_transfer transfer = {0};
    transfer.len = length;
    transfer.rx_buf = (__u64)data_read;
    transfer.tx_buf = (__u64)cmd_write;

    checkls(lr11xx_hal_wait_while_busy(ctx));

    if (transfer.tx_buf != 0) {
        printf("Written: ");
        uint8_t* ptr = (uint8_t*)transfer.tx_buf;
        for (uint32_t i = 0; i < transfer.len; i++) {
            printf("%02hhx ", ptr[i]);
        }
        printf("\n");
    }

    checkls(ioctl(ctx->spi_device, SPI_IOC_MESSAGE(1), transfer) >= 0);

    if (transfer.rx_buf != 0) {        
        printf("Read:    ");
        uint8_t* ptr = (uint8_t*)transfer.rx_buf;
        for (uint32_t i = 0; i < transfer.len; i++) {
            printf("%02hhx ", ptr[i]);
        }
        printf("\n");
    }

    return LR11XX_HAL_STATUS_OK;
}

lr11xx_hal_status_t lr11xx_hal_write(const void *context, const uint8_t *command, const uint16_t command_length, const uint8_t *data, const uint16_t data_length) {
    const lr11xx_hal_context_t* ctx = lr11xx_hal_context_from_ptr(context);

    // calculat packet length
    // command + data + (optional crc)
    __u32 len = command_length + data_length;
    #if defined( USE_LR11XX_CRC_OVER_SPI )
        len += 1;
    #endif

    // construct packet & ioc transfer
    struct spi_ioc_transfer t = {0};
    uint8_t* tx_buf = malloc(len); assert(tx_buf);
    t.tx_buf = (__u64)tx_buf;
    t.len = len;
    memcpy(tx_buf, command, command_length);
    memcpy(tx_buf + command_length, data, data_length);

    #if defined( USE_LR11XX_CRC_OVER_SPI )
        uint8_t cmd_crc = lr11xx_hal_compute_crc( 0xFF, command, command_length );
        cmd_crc         = lr11xx_hal_compute_crc( cmd_crc, data, data_length );
        memcpy(tx_buf + command_length + data_length, &cmd_crc, 1);
    #endif

    // send data
    checkls(lr11xx_hal_send_ioc_transfer(ctx, &t));

    // clean up
    free(tx_buf);

    return LR11XX_HAL_STATUS_OK;
}

lr11xx_hal_status_t lr11xx_hal_read(const void *context, const uint8_t *command, const uint16_t command_length, uint8_t *data, const uint16_t data_length) {
    const lr11xx_hal_context_t* ctx = lr11xx_hal_context_from_ptr(context);

    const uint8_t               dummy_byte     = LR11XX_NOP;

    // calculat tx packet lengths
    // command + (optional crc)
    __u32 tx_len = command_length;
    #if defined( USE_LR11XX_CRC_OVER_SPI )
        tx_len += 1;
    #endif

    // construct cmd packet & ioc transfer
    struct spi_ioc_transfer cmd_tx = {0};
    cmd_tx.len = tx_len;
    uint8_t* tx_buf = malloc(tx_len); assert(tx_buf);
    cmd_tx.tx_buf = (__u64)tx_buf;
    memcpy(tx_buf, command, command_length);
    #if defined( USE_LR11XX_CRC_OVER_SPI )
        const uint8_t cmd_crc = lr11xx_hal_compute_crc( 0xFF, command, command_length );
        memcpy(tx_buf + command_length, &cmd_crc, 1);
    #endif

    // calculat rx packet lengths
    // dummpy byte + data + (optional crc)
    __u32 rx_len = 1 + data_length;
    #if defined( USE_LR11XX_CRC_OVER_SPI )
        rx_len += 1;
    #endif

    // construct receive packet & ioc transfer
    struct spi_ioc_transfer data_rx = {0};
    data_rx.len = rx_len;
    uint8_t* data_rx_buf = malloc(rx_len); assert(data_rx_buf);
    data_rx.rx_buf = (__u64)data_rx_buf;
    uint8_t* nop_tx_buf = malloc(rx_len); assert(nop_tx_buf);
    memset(nop_tx_buf, dummy_byte, rx_len); 
    data_rx.tx_buf = (__u64)nop_tx_buf;

    // transmit cmd packet 
    checkls(lr11xx_hal_send_ioc_transfer(ctx, &cmd_tx));
    usleep(1000);
    // receive data & transmit nop
    checkls(lr11xx_hal_send_ioc_transfer(ctx, &data_rx));

    // parse received data
    memcpy(data, data_rx_buf + 1, data_length);
    #if defined( USE_LR11XX_CRC_OVER_SPI )
        uint8_t dummy = data_rx_buf[0];
        uint8_t crc_rx = data_rx_buf[rx_len - 1];
        uint8_t crc_computed = lr11xx_hal_compute_crc( 0xFF, &dummy, 1 );
        crc_computed         = lr11xx_hal_compute_crc( crc_computed, data, data_length );
    #endif

    // free resources
    free(tx_buf);
    free(data_rx_buf);
    free(nop_tx_buf);

    // check crc if crc enabled
    #if defined( USE_LR11XX_CRC_OVER_SPI )
        if( crc_rx != crc_computed ) {
            return LR11XX_HAL_STATUS_ERROR;
        }
    #endif
    return LR11XX_HAL_STATUS_OK;
}

lr11xx_hal_status_t lr11xx_hal_direct_read(const void *context, uint8_t *data, const uint16_t data_length) {
    const lr11xx_hal_context_t* ctx = lr11xx_hal_context_from_ptr(context);

    // calculate receive packet len
    // data + (optional crc)
    size_t len = data_length;
    #if defined( USE_LR11XX_CRC_OVER_SPI )
        len += 1;
    #endif

    // construct receive packet & ioc transfer
    struct spi_ioc_transfer rx = {0};
    rx.len = len;
    uint8_t* rx_buf = malloc(len); assert(rx_buf);
    rx.rx_buf = (__u64)rx_buf;

    // receive data
    checkls(lr11xx_hal_send_ioc_transfer(ctx, &rx));

    // parse received data
    memcpy(data, rx_buf, data_length);
    #if defined( USE_LR11XX_CRC_OVER_SPI )
        uint8_t rx_crc = rx_buf[len - 1];
        // check crc value
        uint8_t crc_computed = lr11xx_hal_compute_crc( 0xFF, data, data_length );
    #endif

    // free resources
    free(rx_buf);

    // check crc if enabled
    #if defined( USE_LR11XX_CRC_OVER_SPI )
        if( rx_crc != crc_computed ) {
            return LR11XX_HAL_STATUS_ERROR;
        }
    #endif
    return LR11XX_HAL_STATUS_OK;
}

lr11xx_hal_status_t lr11xx_hal_reset(const void *context) {
    const lr11xx_hal_context_t* ctx = lr11xx_hal_context_from_ptr(context);

    checkls(lgGpioWrite(ctx->gpio_device, ctx->reset_pin, 0) == 0);
    usleep(1000); // sleep 1ms for IO to catch up
    checkls(lgGpioWrite(ctx->gpio_device, ctx->reset_pin, 1) == 0);

    return LR11XX_HAL_STATUS_OK;
}

lr11xx_hal_status_t lr11xx_hal_wakeup(const void *context) {
    const lr11xx_hal_context_t* ctx = lr11xx_hal_context_from_ptr(context);

    // checkls(lgGpioWrite(ctx->gpio_device, ctx->nss_pin, 0) == 0);
    const uint8_t cmd[2] = {0};
    size_t bits_per_ms = LR1121_KSPI_MAX_SPEED_HZ / 1000 / 8;
    uint8_t* data = calloc(1, bits_per_ms); // write enough nops for 1ms
    lr11xx_hal_status_t stat = lr11xx_hal_write(ctx, cmd, 2, data, bits_per_ms);
    free(data);
    // usleep(1000);
    // checkls(lgGpioWrite(ctx->gpio_device, ctx->nss_pin, 1) == 0);

    return stat;
}

lr11xx_hal_status_t lr11xx_hal_abort_blocking_cmd(const void *context) {
    const lr11xx_hal_context_t* ctx = lr11xx_hal_context_from_ptr(context);
    uint8_t                     command[4]     = { 0 };

    // build packet & ioc transfer
    struct spi_ioc_transfer t = {0};
    t.tx_buf = (__u64)&command;
    t.len = 4;

    // transmit packet
    // not using lr11xx_hal_send_ioc_transfer as wait busy is comes after instead of before nss to low 
    // checkls(lgGpioWrite(ctx->gpio_device, ctx->nss_pin, 0) == 0);

    checkls(ioctl(ctx->spi_device, SPI_IOC_MESSAGE(1), &t) >= 0);

    // checkls(lgGpioWrite(ctx->gpio_device, ctx->nss_pin, 1) == 0);

    checkls(lr11xx_hal_wait_while_busy(ctx));

    return LR11XX_HAL_STATUS_OK;
}
#endif