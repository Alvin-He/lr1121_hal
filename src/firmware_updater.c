#include "lr11xx_bootloader_types.h"
#include "lr11xx_types.h"
#include <openssl/types.h>
#include <sched.h>
#include <stdbool.h>
#include <string.h>
#undef NDEBUG // To ensure that assert based checks happen

#include <assert.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <lr11xx_bootloader.h>
#include <openssl/evp.h>
#include "lr11xx_linux_hal.h"

#define K_GPIO_DEVICE_PATH "/dev/gpiochip4"
#define K_BOOTSTRAP_MAX_RETRIES 3
#define K_WAIT_TIME_FOR_START_UP_SECONDS 5 
#define K_MIN_ARGS 5
#define K_MAX_ARGS 6

int main(int argc, const char* argv[]) {
    argc -= 1; // remove 1 for programe name argument
    if (!(K_MIN_ARGS <= argc && argc <= K_MAX_ARGS)) {
        printf("Invalid number of arguments: Got %d arguments. Expected min %d, max %d arguments.\n", argc, K_MIN_ARGS, K_MAX_ARGS);
        printf("Usuage: <reset_pin_name> <nss_pin_name> <busy_pin_name> <spi_device_path> <firmware_bin_path> [gpio_chip_path=%s]", K_GPIO_DEVICE_PATH);
        return 1;
    }

    //
    // parse arguments
    //
    const char* reset_pin_name = argv[1];
    const char* nss_pin_name = argv[2];
    const char* busy_pin_name = argv[3];
    const char* spi_device_path = argv[4];
    const char* firmware_bin_path = argv[5];
    const char* gpio_device_path = argc == 6 ? argv[6] : K_GPIO_DEVICE_PATH;

    printf("LR11XX Firmware Updater.\n");

    //
    // Load firmware
    //

    printf("Reading firmware.\n");
    int firmware_fd = open(firmware_bin_path, O_RDONLY);
    if (firmware_fd < 0) {
        printf("Failed to open firmware bin path at: %s.\n", firmware_bin_path);
        perror("Got error:");
        return 1;
    }
    struct stat firmware_meta = {0};
    if (fstat(firmware_fd, &firmware_meta) != 0) {
        printf("Failed to open get firmware file metadata!\n");
        perror("Got error:");
        close(firmware_fd);
        return 1;
    };
    uint32_t* firmware = malloc(firmware_meta.st_size); assert(firmware);
    if (read(firmware_fd, firmware, firmware_meta.st_size) != 0) {
        printf("Failed to read firmware file!\n");
        perror("Got error:");
        free(firmware);
        close(firmware_fd);
        return 1;
    };
    // clean up
    close(firmware_fd); firmware_fd = -1;

    // compute md5 hash of firmware
    const EVP_MD* md5 = EVP_md5();
    uint8_t firmware_hash[EVP_MAX_MD_SIZE] = {0}; // this create a null terminated string if firmware_hash is used as a str reference
    unsigned int hash_len;
    if (EVP_Digest(firmware, firmware_meta.st_size, firmware_hash, &hash_len, md5, NULL) != 1) {
        printf("Failed to compute firmware hash!\n");
        perror("Got error:");
        free(firmware);
        return 1;
    };

    //
    // Verify configuration before proceeding
    //

    printf("LR11XX SPI device is at: %s\n", spi_device_path);
    printf("LR11XX GPIO pins are mapped as following: \n");
    printf("\tRESET\t%s\n", reset_pin_name);
    printf("\tNSS  \t%s\n", nss_pin_name);
    printf("\tBUSY \t%s\n", busy_pin_name);
    printf("Firmware file to flush to LR11XX is: %s\n", firmware_bin_path);
    printf("This firmware file have md5 hash of `%s`.", firmware_hash);
    
    // get confirmation before proceed
    printf("\nPlease confirm all settings are correct.\n");
    printf("After susscessfully confirming all settings: Press Y to begin, or press ANY OTHER KEY to STOP and quit.\n");
    char* line = NULL;
    size_t len = 0;
    if (getline(&line, &len, stdin) == -1
        || len != 1
        || !(line[0] == 'Y' || line[0] == 'y')
    ) {
        printf("Exiting.\n");
        free(line);
        return 0;
    };

    printf("Starting firmware update. Please make sure the LR11XX stays powered on untill this process finishes.\n");


    //
    // Init LR11XX HAL
    //
    printf("Opening LR11XX.\n");
    lr11xx_hal_context_t* ctx = lr11xx_init_hal(spi_device_path, gpio_device_path, reset_pin_name, nss_pin_name, busy_pin_name);


    //
    // Bootstrapping 
    //
    bool is_in_bootloader_mode = false;
    for (int i = 0; i < K_BOOTSTRAP_MAX_RETRIES; i++) {
        printf("Booting LR11XX into bootloader mode. Try %d of %d.\n", i, K_BOOTSTRAP_MAX_RETRIES);
        
        if (lr11xx_bootloader_reboot(ctx, true) != LR11XX_STATUS_OK) {
            sleep(K_WAIT_TIME_FOR_START_UP_SECONDS);
            continue;
        };

        sleep(K_WAIT_TIME_FOR_START_UP_SECONDS); // wait for LR11XX to start

        lr11xx_bootloader_stat1_t s1;
        lr11xx_bootloader_stat2_t s2;
        lr11xx_bootloader_irq_mask_t irq;
        if (lr11xx_bootloader_get_status(ctx, &s1, &s2, &irq) != LR11XX_STATUS_OK) continue;

        if (!s2.is_running_from_flash) {
            is_in_bootloader_mode = true;
            break;
        }    
    }
    if (!is_in_bootloader_mode) {
        printf("CRITICAL: Failed to boot into bootloader mode!");
        free(firmware);
        lr11xx_close_hal(ctx);
        return 1;
    }
    printf("Booted LR11XX into bootloader mode.\n");

    //
    // Erase flash
    //
    printf("Erasing LR11XX flash.\n");
    if (lr11xx_bootloader_erase_flash(ctx) != LR11XX_STATUS_OK) {
        printf("CRITICAL: Erase Flash returned error!");
        free(firmware);
        lr11xx_close_hal(ctx);
        return 1;
    };

    //
    // Flash new firmware
    //
    printf("Flashing new firmware.\n");
    if (lr11xx_bootloader_write_flash_encrypted_full(ctx, 0, firmware, firmware_meta.st_size/4) != LR11XX_STATUS_OK) {
        printf("CRITICAL: Flash firmware returned error!");
        free(firmware);
        lr11xx_close_hal(ctx);
        return 1;
    };
    printf("Finished flashing firmware.\n");
    // clean up
    free(firmware);

    //
    // Reboot and verify
    //
    printf("Rebooting LR11XX to verify firmware.\n");
    if (lr11xx_bootloader_reboot(ctx, false) != LR11XX_STATUS_OK) {
        printf("CRITICAL: Failed to reboot LR11XX!");
        lr11xx_close_hal(ctx);
        return 1;
    };
    sleep(K_WAIT_TIME_FOR_START_UP_SECONDS); // wait for LR11XX to start
    
    // check status
    lr11xx_bootloader_stat1_t s1;
    lr11xx_bootloader_stat2_t s2;
    lr11xx_bootloader_irq_mask_t irq;
    if (lr11xx_bootloader_get_status(ctx, &s1, &s2, &irq) != LR11XX_STATUS_OK) {
        printf("CRITICAL: Failed to get status of LR11XX!");
        lr11xx_close_hal(ctx);
        return 10;
    };
    if (!s2.is_running_from_flash) {
        printf("Failure: LR11XX booted back into bootloader mode.\n");
        printf("This means that the LR11XX couldn't verify the firmware flushed or some other error occoured.\n");
        printf("You may try to rerun this firmware updater after a powercycle of the LR11XX.\n");
        lr11xx_close_hal(ctx);
        return 10;
    }
    printf("LR11XX booted back sussccessfully to running onboard flashed firmware.");

    // verify loaded firmware hash
    lr11xx_bootloader_hash_t hash = {0};
    if (lr11xx_bootloader_get_hash(ctx, hash) != LR11XX_STATUS_OK) {
        printf("WARNING: LR11XX booted back sussccessfully, but we were unable to get hash of flashed firmware.\n");
        printf("If you wish to, you may try to rerun this firmware updater after a powercycle of the LR11XX.\n");
        printf("Exiting: Unable to veify flushed image.\n");
        lr11xx_close_hal(ctx);
        return 10;
    };

    // free resouces
    lr11xx_close_hal(ctx);

    if (memcmp(hash, firmware_hash, sizeof(hash)) == 0) {
        printf("Susscessfully verified flushed firmware hash!\n");
        printf("LR11XX firmware update susscessful!\n");
        return 0;
    } else {
        printf("WARNING: LR11XX booted back sussccessfully, but the firmware hashs were found to be different.\n");
        printf("If you wish to, you may try to rerun this firmware updater after a powercycle of the LR11XX.\n");
        printf("Exiting: Unable to veify flushed image.\n");
        return 10;
    };
}