// Raspberry-PI or generic linux implenmentation based on 
// SWSD003's HAL for STM32: https://github.com/Lora-net/SWSD003/blob/08912a2324bfc931224d368984b58b4a853078ad/lr11xx/common/lr11xx_hal.c

#ifndef LR11XX_LINUX_HAL_H
#define LR11XX_LINUX_HAL_H


//////////////////////////////
/// Advanced configuration ///
//////////////////////////////
// There are bascially constants, don't change them unless you know what you are doing

//#define USE_LR11XX_CRC_OVER_SPI
#include "lr11xx_hal.h"
#include <openssl/cryptoerr_legacy.h>
#include <stdint.h>
#ifndef LR1121_KSPI_MODE // uint8_t
    #define LR1121_KSPI_MODE SPI_MODE_0
#endif
#ifndef LR1121_KSPI_MAX_SPEED_HZ // uint32_t
    // #define LR1121_KSPI_MAX_SPEED_HZ 16000000 // 16mhz max SPI speed, see LR1121 datasheet, rev 2, page 23 
    #define LR1121_KSPI_MAX_SPEED_HZ 100000 // 16mhz max SPI speed, see LR1121 datasheet, rev 2, page 23 
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

typedef struct lr11xx_hal_context {
    int spi_device;
    int gpio_device;
    unsigned int reset_pin;
    unsigned int nss_pin;
    unsigned int busy_pin;

} lr11xx_hal_context_t;

/**
 * @brief initializes the HAL interface for lr11xx
 *
 * @remark All instances of HAL initilized must be closed through @ref lr11xx_close_hal
 */
lr11xx_hal_context_t* lr11xx_init_hal(
    const char* spi_device_path, const int gpio_chip_number,
    const int reset_pin, const int nss_pin, const int busy_pin
);

/**
 * @brief Closes a HAL instance, and release all SPI & GPIO resources
 */
void lr11xx_close_hal(lr11xx_hal_context_t* ctx);

lr11xx_hal_status_t lr11xx_boostrap(lr11xx_hal_context_t* ctx);
bool lr11xx_hal_wait_while_busy(const lr11xx_hal_context_t* ctx);
lr11xx_hal_status_t lr11xx_hal_direct_read_write(const void *context, const uint8_t *cmd_write, uint8_t *data_read, const uint16_t length);
#endif