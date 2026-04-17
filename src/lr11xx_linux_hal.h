// Raspberry-PI or generic linux implenmentation based on 
// SWSD003's HAL for STM32: https://github.com/Lora-net/SWSD003/blob/08912a2324bfc931224d368984b58b4a853078ad/lr11xx/common/lr11xx_hal.c

#ifndef LR11XX_LINUX_HAL_H
#define LR11XX_LINUX_HAL_H


//////////////////////////////
/// Advanced configuration ///
//////////////////////////////
// There are bascially constants, don't change them unless you know what you are doing

//#define USE_LR11XX_CRC_OVER_SPI
#ifndef LR1121_KSPI_MODE // uint8_t
    #define LR1121_KSPI_MODE SPI_MODE_0
#endif
#ifndef LR1121_KSPI_MAX_SPEED_HZ // uint32_t
    #define LR1121_KSPI_MAX_SPEED_HZ 16000000 // 16mhz max SPI speed, see LR1121 datasheet, rev 2, page 23 
#endif 
#ifndef LR1121_KBITS_PER_WORD // uint8_t
    #define LR1121_KBITS_PER_WORD 0 // number of bits per word. 0 means 8 bits  
#endif 
#ifndef LR1121_KSPI_IS_LSB_FIRST // uint8_t
    #define LR1121_KSPI_IS_LSB_FIRST 0 // MSB first (0 is MSB, 1 or else is LSB)
#endif
#ifndef LR1121_KGPIO_CONSUMER_IDENT
    #define LR1121_KGPIO_CONSUMER_IDENT "LR11XX_GPIO_CONSUMER"
#endif 

typedef struct lr11xx_hal_context_tracked_memory {
    struct gpiod_chip* chip; 
    struct gpiod_line_settings* settings_input;
    struct gpiod_line_settings* settings_output;
    struct gpiod_line_config* cfg_line;
    struct gpiod_request_config* cfg_req;
    struct gpiod_line_request* line_req;
} lr11xx_hal_ctx_mem_t;

typedef struct lr11xx_hal_context {
    lr11xx_hal_ctx_mem_t* mem;

    int spi_device;
    unsigned int reset_pin_offset;
    unsigned int nss_pin_offset;
    unsigned int busy_pin_offset;

    struct gpiod_line_request* line_req;
} lr11xx_hal_context_t;

/**
 * @brief initializes the HAL interface for lr11xx
 *
 * @remark All instances of HAL initilized must be closed through @ref lr11xx_close_hal
 */
lr11xx_hal_context_t* lr11xx_init_hal(
    const char* spi_device_path, const char* gpio_device_path,
    const char* reset_pin_name, const char* nss_pin_name, const char* busy_pin_name
);

/**
 * @brief Closes a HAL instance, and release all SPI & GPIO resources
 */
void lr11xx_close_hal(lr11xx_hal_context_t* ctx);

#endif