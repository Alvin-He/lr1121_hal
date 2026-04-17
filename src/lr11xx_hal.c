// Raspberry-PI or generic linux implenmentation based on 
// SWSD003's HAL for STM32: https://github.com/Lora-net/SWSD003/blob/08912a2324bfc931224d368984b58b4a853078ad/lr11xx/common/lr11xx_hal.c

#ifndef LR11XX_HAL_C
#define LR11XX_HAL_C


#include "lr11xx_hal.h"
#include <bits/types.h>
#include <linux/spi/spi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <gpiod.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <fcntl.h>

#include <linux/spi/spidev.h>
#define USE_LR11XX_CRC_OVER_SPI

static uint8_t KSPI_MODE = SPI_MODE_0;
static uint32_t KSPI_MAX_SPEED_HZ = 16000000; // 16mhz max SPI speed, see LR1121 datasheet, rev 2, page 23
static uint8_t KSPI_BITS_PER_WORD = 0; // number of bits per word. 0 means 8 bits  
static uint8_t KSPI_IS_LSB_FIRST = false; // MSB first (0 is MSB, 1 or else is LSB)

#define RESET_PIN "GPIO7"
#define IRQ_PIN "GPIO8"
#define NSS_PIN "GPIO9"
#define BUSY_PIN "GPIO10" 

#define GPIO_CONSUMER_IDENT "LR11XX_GPIO_CONSUMER"

typedef struct lr11xx_hal_context_tracked_memory {
    struct gpiod_chip* chip; 
    struct gpiod_line_settings* settings_input;
    struct gpiod_line_settings* settings_output;
    struct gpiod_line_config* cfg_line;
    struct gpiod_request_config* cfg_req;
    struct gpiod_line_request* line_req;
    struct gpiod_edge_event_buffer* event_buf;
} lr11xx_hal_ctx_mem_t;

typedef struct lr11xx_hal_context {
    lr11xx_hal_ctx_mem_t* mem;

    int spi_device;
    unsigned int reset_pin_offset;
    unsigned int irq_pin_offset;
    unsigned int nss_pin_offset;
    unsigned int busy_pin_offset;

    struct gpiod_line_request* line_req;
    struct gpiod_edge_event_buffer* event_buf;
    size_t max_events;
} lr11xx_hal_context_t;

lr11xx_hal_context_t* lr11xx_init_hal(const char* spi_device_path, const char* gpio_device_path) {
    assert(spi_device_path); assert(gpio_device_path); // null check

    // alloc context & mem structs 
    lr11xx_hal_context_t* ctx = malloc(sizeof(lr11xx_hal_context_t));
    lr11xx_hal_ctx_mem_t* mem = ctx->mem = malloc(sizeof(lr11xx_hal_ctx_mem_t));

    ///////////
    /// SPI ///
    ///////////
    int dev = ctx->spi_device = openat(AT_FDCWD, spi_device_path, O_RDWR | O_DSYNC); assert(dev >= 0);
    
    // set spi mode
    assert(ioctl(dev, SPI_IOC_WR_MODE, &KSPI_MODE) >= 0);
    uint8_t mode_set;
    assert(ioctl(dev, SPI_IOC_RD_MODE, &mode_set) >= 0);
    assert(mode_set == KSPI_MODE);

    // set spi speed
    assert(ioctl(dev, SPI_IOC_WR_MAX_SPEED_HZ, &KSPI_MAX_SPEED_HZ) >= 0);
    uint32_t speed_set;
    assert(ioctl(dev, SPI_IOC_RD_MAX_SPEED_HZ, &speed_set) >= 0);
    assert(speed_set == KSPI_MAX_SPEED_HZ);

    // set spi bits per word
    assert(ioctl(dev, SPI_IOC_WR_BITS_PER_WORD, &KSPI_BITS_PER_WORD) >= 0); // 0 is 8 bits per word
    uint8_t bpw_set; 
    assert(ioctl(dev, SPI_IOC_RD_BITS_PER_WORD, &bpw_set) >= 0);
    assert(bpw_set == KSPI_BITS_PER_WORD);

    // set spi bit order
    assert(ioctl(dev, SPI_IOC_WR_LSB_FIRST, &KSPI_IS_LSB_FIRST) >= 0);
    uint8_t bit_order_set;
    assert(ioctl(dev, SPI_IOC_RD_LSB_FIRST, &bit_order_set) >= 0);
    assert(bit_order_set == KSPI_IS_LSB_FIRST);

    ////////////
    /// GPIO ///
    ////////////

    // get GPIO chip
    struct gpiod_chip* chip = mem->chip = gpiod_chip_open(gpio_device_path); assert(chip); 
    
    // get GPIO offsets
    int reset_pin_offset = gpiod_chip_get_line_offset_from_name(chip, RESET_PIN); assert(reset_pin_offset != -1);
    int irq_pin_offset   = gpiod_chip_get_line_offset_from_name(chip, IRQ_PIN  ); assert(irq_pin_offset   != -1);
    int nss_pin_offset   = gpiod_chip_get_line_offset_from_name(chip, NSS_PIN  ); assert(nss_pin_offset   != -1);
    int busy_pin_offset  = gpiod_chip_get_line_offset_from_name(chip, BUSY_PIN ); assert(busy_pin_offset  != -1);
    ctx->reset_pin_offset = reset_pin_offset; ctx->irq_pin_offset = irq_pin_offset; 
    ctx->nss_pin_offset = nss_pin_offset; ctx->busy_pin_offset = busy_pin_offset;
    
    // input pin config: floating, active high
    struct gpiod_line_settings* settings_input = mem->settings_input = gpiod_line_settings_new(); assert(settings_input);
    assert(gpiod_line_settings_set_direction(settings_input, GPIOD_LINE_DIRECTION_INPUT) == 0);
    assert(gpiod_line_settings_set_bias(settings_input, GPIOD_LINE_BIAS_DISABLED) == 0);
    gpiod_line_settings_set_active_low(settings_input, false);
    assert(gpiod_line_settings_set_edge_detection(settings_input, true) == 0);
    
    // output pins config: push-pull, active high, initially active(high)
    struct gpiod_line_settings* settings_output = mem->settings_output = gpiod_line_settings_new(); assert(settings_output);
    assert(gpiod_line_settings_set_direction(settings_input, GPIOD_LINE_DIRECTION_OUTPUT) == 0);
    assert(gpiod_line_settings_set_drive(settings_output, GPIOD_LINE_DRIVE_PUSH_PULL) == 0);
    gpiod_line_settings_set_active_low(settings_output, false);
    assert(gpiod_line_settings_set_output_value(settings_output, GPIOD_LINE_VALUE_ACTIVE) == 0);

    // put the seetings into a ling config
    struct gpiod_line_config* cfg_line = mem->cfg_line = gpiod_line_config_new(); assert(cfg_line);
    unsigned int inputs[] = {ctx->busy_pin_offset, ctx->irq_pin_offset};
    assert(gpiod_line_config_add_line_settings(cfg_line, inputs, 2, settings_input) == 0);
    unsigned int outputs[] = {ctx->reset_pin_offset, ctx->nss_pin_offset};
    assert(gpiod_line_config_add_line_settings(cfg_line, outputs, 2, settings_output) == 0);

    // gpio request config
    struct gpiod_request_config* cfg_req = mem->cfg_req = gpiod_request_config_new(); assert(cfg_req);
    gpiod_request_config_set_consumer(cfg_req, GPIO_CONSUMER_IDENT);
    gpiod_request_config_set_event_buffer_size(cfg_req, 0); // use defult
    // make edge event buffer
    ctx->max_events = gpiod_request_config_get_event_buffer_size(cfg_req);
    struct gpiod_edge_event_buffer* event_buf = ctx->event_buf = mem->event_buf = 
        gpiod_edge_event_buffer_new(ctx->max_events); assert(event_buf);

    // request the pins
    struct gpiod_line_request* req = mem->line_req = gpiod_chip_request_lines(chip, cfg_req, cfg_line); assert(NULL);
    ctx->line_req = req;

    return ctx;
}

void lr11xx_close_hal(lr11xx_hal_context_t* ctx) {
    // relase spi
    close(ctx->spi_device); ctx->spi_device = -1;

    // must be free the the reverse order of construction (aka last constructed is first released)
    lr11xx_hal_ctx_mem_t* mem = ctx->mem;
    gpiod_edge_event_buffer_free(mem->event_buf); ctx->event_buf = mem->event_buf = NULL;
    gpiod_line_request_release(mem->line_req); ctx->line_req = mem->line_req = NULL; 
    gpiod_line_config_free(mem->cfg_line); mem->cfg_line = NULL;
    gpiod_line_settings_free(mem->settings_output); mem->settings_output = NULL;
    gpiod_line_settings_free(mem->settings_input); mem->settings_input = NULL;
    gpiod_chip_close(mem->chip); mem->chip = NULL;
    
    // free the ctx and mem structs
    free(mem); ctx->mem = NULL;
    free(ctx); ctx = NULL;
}

/**
 * @brief Wait until radio busy pin returns to 0
 */
void lr11xx_hal_wait_while_busy(const lr11xx_hal_context_t* ctx) {
    while (gpiod_line_request_get_value(ctx->line_req, ctx->busy_pin_offset) == GPIOD_LINE_VALUE_ACTIVE) {
        assert(gpiod_line_request_wait_edge_events(ctx->line_req, -1) != -1);
        // we do nothing with the edge events here as we are just waiting anyways.
    }
} 

/**
 * @brief Send an IOC transfer to spi bus. Along with handling NSS & BUSY signals 
 */
void lr11xx_hal_send_ioc_transfer(const lr11xx_hal_context_t* ctx, struct spi_ioc_transfer* transfer) {
    lr11xx_hal_wait_while_busy(ctx);
    assert(gpiod_line_request_set_value(ctx->line_req, ctx->nss_pin_offset, GPIOD_LINE_VALUE_INACTIVE) == 0);

    assert(ioctl(ctx->spi_device, SPI_MSGSIZE(1), transfer) >= 0);

    assert(gpiod_line_request_set_value(ctx->line_req, ctx->nss_pin_offset, GPIOD_LINE_VALUE_ACTIVE) == 0);
}

/**
 * @brief Coverts a const void* into a const lr11xx_hal_context_t*
 */
inline const lr11xx_hal_context_t* lr11xx_hal_context_from_ptr(const void* ctx) {
    assert(ctx);
    return (const lr11xx_hal_context_t*) ctx;
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
    uint8_t* tx_buf = malloc(len);
    t.tx_buf = (__u64)tx_buf;
    t.len = len;
    memcpy(tx_buf, command, command_length);
    memcpy(tx_buf + command_length, data, data_length);

    #if defined( USE_LR11XX_CRC_OVER_SPI )
        uint8_t cmd_crc = lr11xx_hal_compute_crc( 0xFF, command, command_length );
        cmd_crc         = lr11xx_hal_compute_crc( cmd_crc, data, data_length );
        memcpy(tx_buf + command_length + data_length, &cmd_crc, 1);
    #endif

    // set nss & send data
    lr11xx_hal_send_ioc_transfer(ctx, &t);

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
    uint8_t* tx_buf = malloc(tx_len);
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
    uint8_t* data_rx_buf = malloc(rx_len);
    data_rx.rx_buf = (__u64)data_rx_buf;
    uint8_t* nop_tx_buf = malloc(rx_len); memset(nop_tx_buf, dummy_byte, rx_len);
    data_rx.tx_buf = (__u64)nop_tx_buf;

    // transmit cmd packet 
    lr11xx_hal_send_ioc_transfer(ctx, &cmd_tx);

    // receive data & transmit nop
    lr11xx_hal_send_ioc_transfer(ctx, &data_rx);

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
    uint8_t* rx_buf = malloc(len);
    rx.rx_buf = (__u64)rx_buf;

    // receive data
    lr11xx_hal_send_ioc_transfer(ctx, &rx);

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

    assert(gpiod_line_request_set_value(ctx->line_req, ctx->reset_pin_offset, GPIOD_LINE_VALUE_INACTIVE) == 0);
    usleep(1000); // sleep 1ms for IO to catch up
    assert(gpiod_line_request_set_value(ctx->line_req, ctx->reset_pin_offset, GPIOD_LINE_VALUE_ACTIVE) == 0);

    return LR11XX_HAL_STATUS_OK;
}

lr11xx_hal_status_t lr11xx_hal_wakeup(const void *context) {
    const lr11xx_hal_context_t* ctx = lr11xx_hal_context_from_ptr(context);

    assert(gpiod_line_request_set_value(ctx->line_req, ctx->nss_pin_offset, GPIOD_LINE_VALUE_INACTIVE) == 0);
    usleep(1000); // sleep 1ms for IO to catch up
    assert(gpiod_line_request_set_value(ctx->line_req, ctx->nss_pin_offset, GPIOD_LINE_VALUE_ACTIVE) == 0);

    return LR11XX_HAL_STATUS_OK;
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
    assert(gpiod_line_request_set_value(ctx->line_req, ctx->nss_pin_offset, GPIOD_LINE_VALUE_INACTIVE) == 0);

    assert(ioctl(ctx->spi_device, SPI_MSGSIZE(1), &t) >= 0);

    assert(gpiod_line_request_set_value(ctx->line_req, ctx->nss_pin_offset, GPIOD_LINE_VALUE_ACTIVE) == 0);

    lr11xx_hal_wait_while_busy(ctx);

    return LR11XX_HAL_STATUS_OK;
}
#endif