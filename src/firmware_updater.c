#include <time.h>
#undef NDEBUG // To ensure that assert based checks happen

#include "lr11xx_bootloader_types.h"
#include "lr11xx_hal.h"
#include "lr11xx_system.h"
#include "lr11xx_system_types.h"
#include "lr11xx_types.h"
#include <openssl/types.h>
#include <sched.h>
#include <stdbool.h>
#include <string.h>
// #include "lr1121_loader_2100.h"

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

#define K_GPIO_DEVICE_PATH "4" //"/dev/gpiochip4"
#define K_BOOTSTRAP_MAX_RETRIES 3
#define K_WAIT_TIME_FOR_START_UP_SECONDS 1
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
    uint32_t word_count = firmware_meta.st_size / 4;
    if (word_count * 4 != firmware_meta.st_size) {
        word_count += 1; // didn't cleanly divide, add an extra word to pad out any remaining bytes
    }
    uint32_t* firmware = calloc(word_count, sizeof(uint32_t)); assert(firmware);

    if (read(firmware_fd, firmware, firmware_meta.st_size) < 0) {
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
    uint8_t firmware_hash[EVP_MAX_MD_SIZE] = {0}; 
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
    printf("This firmware file have md5 hash of ");
    for (unsigned int i = 0; i < hash_len; i++) {
        printf("%x", firmware_hash[i]);
    }
    
    // get confirmation before proceed
    printf("\nPlease confirm all settings are correct.\n");
    printf("After susscessfully confirming all settings: Press Y to begin, or press ANY OTHER KEY to STOP and quit.\n");
    // char* line = NULL;
    // size_t size = 0;
    // if (getline(&line, &size, stdin) == -1
    //     || strlen(line) < 1
    //     || !(line[0] == 'Y' || line[0] == 'y')
    // ) {
    //     printf("Exiting.\n");
    //     free(line);
    //     return 0;
    // };

    printf("Starting firmware update. Please make sure the LR11XX stays powered on untill this process finishes.\n");

    //
    // Init LR11XX HAL
    //
    printf("Opening LR11XX.\n");
    lr11xx_hal_context_t* ctx = lr11xx_init_hal(spi_device_path, 
        atoi(gpio_device_path), 
        atoi(reset_pin_name),
        atoi(nss_pin_name),
        atoi(busy_pin_name)
    );

{
    lr11xx_system_stat1_t s1 = {0};
    lr11xx_system_stat2_t s2 = {0};
    lr11xx_system_irq_mask_t irq = {0};
    if (lr11xx_system_get_status(ctx, &s1, &s2, &irq) != LR11XX_STATUS_OK) {
        printf("CRITICAL: Failed to get status of LR11XX!\n");
        lr11xx_close_hal(ctx);
        return 10;
    };
    printf("\tCMD Status:  \t%d\n", s1.command_status);
    printf("\tHas IRQ:     \t%d\n", s1.is_interrupt_active);
    printf("\tIs Flash:    \t%d\n", s2.is_running_from_flash);
    printf("\tChip Mode:   \t%d\n", s2.chip_mode);
    printf("\tReset Status:\t%d\n", s2.reset_status);
}

    //
    // Bootstrapping 
    //
    bool is_in_bootloader_mode = false;
    for (int i = 1; i <= K_BOOTSTRAP_MAX_RETRIES; i++) {
        printf("Booting LR11XX into bootloader mode. Try %d of %d.\n", i, K_BOOTSTRAP_MAX_RETRIES);
        
        if (lr11xx_bootloader_reboot(ctx, true) != LR11XX_STATUS_OK) {
            printf("bootloader_reboot cmd failed\n");
            sleep(K_WAIT_TIME_FOR_START_UP_SECONDS);
            continue;
        };

        printf("Booted out of bootloader!\n");

        lr11xx_system_version_t ver;
        assert(lr11xx_system_get_version(ctx, &ver) == LR11XX_STATUS_OK);
        printf("\tFW:\t%02x\n", ver.fw);
        printf("\tHW:\t%02x\n", ver.hw);
        printf("\tType:\t%02x\n", ver.type);

        // if (lr11xx_boostrap(ctx) != LR11XX_HAL_STATUS_OK) {
        //     printf("bootloader_reboot cmd failed\n");
        //     sleep(K_WAIT_TIME_FOR_START_UP_SECONDS);
        //     continue;
        // };

        sleep(K_WAIT_TIME_FOR_START_UP_SECONDS); // wait for LR11XX to start

        lr11xx_bootloader_stat1_t s1;
        lr11xx_bootloader_stat2_t s2;
        lr11xx_bootloader_irq_mask_t irq;
        printf("get status\n");
        if (lr11xx_bootloader_get_status(ctx, &s1, &s2, &irq) != LR11XX_STATUS_OK) continue;

        printf("\tCMD Status:  \t%02hhx\n", s1.command_status);
        printf("\tHas IRQ:     \t%02hhx\n", s1.is_interrupt_active);
        printf("\tIs Flash:    \t%02hhx\n", s2.is_running_from_flash);
        printf("\tChip Mode:   \t%02hhx\n", s2.chip_mode);
        printf("\tReset Status:\t%02hhx\n", s2.reset_status);

        if (!s2.is_running_from_flash) {
            is_in_bootloader_mode = true;
            break;
        }
    }
    if (!is_in_bootloader_mode) {
        printf("CRITICAL: Failed to boot into bootloader mode!\n");
        free(firmware);
        lr11xx_close_hal(ctx);
        return 1;
    }
    printf("Booted LR11XX into bootloader mode.\n");
    
    printf("bootloader get eui\n");
    lr11xx_bootloader_chip_eui_t eui;
    assert(lr11xx_bootloader_read_chip_eui(ctx, eui) == LR11XX_STATUS_OK);
    printf("eui: ");
    for (size_t i = 0; i < sizeof(eui); i++) {
        printf("%02hhx ", eui[i]);
    }
    printf("\n");
{
    printf("literal get-status: \n");
    const uint8_t status_cmd[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t status_res[6] = {0};
    assert(lr11xx_hal_direct_read_write(ctx, status_cmd, status_res, 6) == LR11XX_HAL_STATUS_OK);
    printf("get-status result: ");
    for (size_t i = 0; i < sizeof(status_res); i++) {
        printf("%02hhx ", status_res[i]);
    }
    printf("\n");
}
{
    printf("Send Get Errors:\n");
    lr11xx_system_errors_t errors;
    assert(lr11xx_system_get_errors(ctx, &errors) == LR11XX_STATUS_OK);
    printf("Got Errors: %02hx\n", errors);
}

    printf("Clear Errors:\n");
    assert(lr11xx_system_clear_errors(ctx) == LR11XX_STATUS_OK);

// {
//     printf("Send GetVersion CMD\n");
//     const uint8_t get_version_cmd[2] = {0x01, 0x01};
//     assert(lr11xx_hal_direct_read_write(ctx, get_version_cmd, NULL, 2) == LR11XX_HAL_STATUS_OK);

//     printf("literal get-status: \n");
//     const uint8_t status_cmd[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
//     uint8_t status_res[6] = {0};
//     assert(lr11xx_hal_direct_read_write(ctx, status_cmd, status_res, 6) == LR11XX_HAL_STATUS_OK);
//     printf("get-status result: ");
//     for (size_t i = 0; i < sizeof(status_res); i++) {
//         printf("%02hhx ", status_res[i]);
//     }
//     printf("\n");
// }

    printf("boot strappping again\n");
    assert(lr11xx_boostrap(ctx) == LR11XX_HAL_STATUS_OK);

{
    printf("literal get-status: \n");
    const uint8_t status_cmd[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t status_res[6] = {0};
    assert(lr11xx_hal_direct_read_write(ctx, status_cmd, status_res, 6) == LR11XX_HAL_STATUS_OK);
    printf("get-status result: ");
    for (size_t i = 0; i < sizeof(status_res); i++) {
        printf("%02hhx ", status_res[i]);
    }
    printf("\n");
}

{
    printf("Send Get Errors:\n");
    lr11xx_system_errors_t errors;
    assert(lr11xx_system_get_errors(ctx, &errors) == LR11XX_STATUS_OK);
    printf("Got Errors: %02hx\n", errors);
}

{
    printf("literal get-status: \n");
    const uint8_t status_cmd[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t status_res[6] = {0};
    assert(lr11xx_hal_direct_read_write(ctx, status_cmd, status_res, 6) == LR11XX_HAL_STATUS_OK);
    printf("get-status result: ");
    for (size_t i = 0; i < sizeof(status_res); i++) {
        printf("%02hhx ", status_res[i]);
    }
    printf("\n");
}
{
    printf("literal get-status: \n");
    const uint8_t status_cmd[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t status_res[6] = {0};
    assert(lr11xx_hal_direct_read_write(ctx, status_cmd, status_res, 6) == LR11XX_HAL_STATUS_OK);
    printf("get-status result: ");
    for (size_t i = 0; i < sizeof(status_res); i++) {
        printf("%02hhx ", status_res[i]);
    }
    printf("\n");
}


// {
//     printf("Send 0x80 0x00 CMD\n");
//     const uint8_t cmd[2] = {0x80, 0x00};
//     assert(lr11xx_hal_direct_read_write(ctx, cmd, NULL, 2) == LR11XX_HAL_STATUS_OK);

//     printf("literal get-status: \n");
//     const uint8_t status_cmd[6] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
//     uint8_t status_res[6] = {0};
//     assert(lr11xx_hal_direct_read_write(ctx, status_cmd, status_res, 6) == LR11XX_HAL_STATUS_OK);
//     printf("get-status result: ");
//     for (size_t i = 0; i < sizeof(status_res); i++) {
//         printf("%02hhx ", status_res[i]);
//     }
//     printf("\n");
// }

    printf("bootloader get ver\n");
    lr11xx_bootloader_version_t ver1 = {0};
    assert(lr11xx_bootloader_get_version(ctx, &ver1) == LR11XX_STATUS_OK);
    printf("\tFW:\t%d\n", ver1.fw);
    printf("\tHW:\t%d\n", ver1.hw);
    printf("\tType:\t%x\n", ver1.type);
    
    if (ver1.type != 0xDF) {
        printf("Incompitable bootloader type, exiting.\n");
        free(firmware);
        lr11xx_close_hal(ctx);
        return 1;
    }

    //
    // Erase flash
    //
    printf("Erasing LR11XX flash.\n");
    if (lr11xx_bootloader_erase_flash(ctx) != LR11XX_STATUS_OK) {
        printf("CRITICAL: Erase Flash returned error!\n");
        free(firmware);
        lr11xx_close_hal(ctx);
        return 1;
    };
    {
        lr11xx_bootloader_stat1_t s1;
        lr11xx_bootloader_stat2_t s2;
        lr11xx_bootloader_irq_mask_t irq;
        printf("get status\n");
        assert(lr11xx_bootloader_get_status(ctx, &s1, &s2, &irq) == LR11XX_STATUS_OK);
        printf("\tCMD Status:  \t%02hhx\n", s1.command_status);
        printf("\tHas IRQ:     \t%02hhx\n", s1.is_interrupt_active);
        printf("\tIs Flash:    \t%02hhx\n", s2.is_running_from_flash);
        printf("\tChip Mode:   \t%02hhx\n", s2.chip_mode);
        printf("\tReset Status:\t%02hhx\n", s2.reset_status);

        if (s1.command_status != LR11XX_BOOTLOADER_CMD_STATUS_OK) {
            printf("failed to erase chip\n");
            free(firmware);
            lr11xx_close_hal(ctx);
            return 1;
        }
    }

    //
    // Flash new firmware
    //
    printf("Flashing new firmware.\n");
    if (lr11xx_bootloader_write_flash_encrypted_full(ctx, 0, firmware, word_count) != LR11XX_STATUS_OK) {
        printf("CRITICAL: Flash firmware returned error!\n");
        free(firmware);
        lr11xx_close_hal(ctx);
        return 1;
    };
    printf("Finished flashing firmware.\n");
    // clean up
    free(firmware);

    {
        lr11xx_bootloader_stat1_t s1;
        lr11xx_bootloader_stat2_t s2;
        lr11xx_bootloader_irq_mask_t irq;
        printf("get status post write\n");
        assert(lr11xx_bootloader_get_status(ctx, &s1, &s2, &irq) == LR11XX_STATUS_OK);

        printf("\tCMD Status:  \t%02hhx\n", s1.command_status);
        printf("\tHas IRQ:     \t%02hhx\n", s1.is_interrupt_active);
        printf("\tIs Flash:    \t%02hhx\n", s2.is_running_from_flash);
        printf("\tChip Mode:   \t%02hhx\n", s2.chip_mode);
        printf("\tReset Status:\t%02hhx\n", s2.reset_status);
    }

    //
    // Verify flashed firmware
    //
    lr11xx_bootloader_hash_t hash = {0};
    if (lr11xx_bootloader_get_hash(ctx, hash) != LR11XX_STATUS_OK) {
        printf("CRITICAL: LR1121 flahsed susscessfully, but we were unable to get hash of flashed firmware.\n");
        printf("If you wish to, you may try to rerun this firmware updater after a powercycle of the LR11XX.\n");
        lr11xx_close_hal(ctx);
        return 10;
    }

    if (memcmp(hash, firmware_hash, sizeof(hash)) == 0) {
        printf("Susscessfully verified flushed firmware hash!\n");
    } else {
        printf("WARNING: LR11XX flashed susscessfully, but the firmware hashs were found to be different.\n");
        printf("If you wish to, you may try to rerun this firmware updater after a powercycle of the LR11XX.\n");
    };


    //
    // Reboot and verify
    //
    printf("Rebooting LR11XX with new firmware.\n");
    if (lr11xx_bootloader_reboot(ctx, false) != LR11XX_STATUS_OK) {
        printf("CRITICAL: Failed to reboot LR11XX!");
        lr11xx_close_hal(ctx);
        return 1;
    };
    sleep(K_WAIT_TIME_FOR_START_UP_SECONDS); // wait for LR11XX to start

    lr11xx_system_version_t ver = {0};
    assert(lr11xx_system_get_version(ctx, &ver) == LR11XX_STATUS_OK);
    printf("\tFW:\t%d\n", ver.fw);
    printf("\tHW:\t%d\n", ver.hw);
    printf("\tType:\t%x\n", ver.type);

    // check status
    lr11xx_system_stat1_t s1 = {0};
    lr11xx_system_stat2_t s2 = {0};
    lr11xx_system_irq_mask_t irq = {0};
    if (lr11xx_system_get_status(ctx, &s1, &s2, &irq) != LR11XX_STATUS_OK) {
        printf("CRITICAL: Failed to get status of LR11XX!\n");
        lr11xx_close_hal(ctx);
        return 10;
    };
    printf("\tCMD Status:  \t%d\n", s1.command_status);
    printf("\tHas IRQ:     \t%d\n", s1.is_interrupt_active);
    printf("\tIs Flash:    \t%d\n", s2.is_running_from_flash);
    printf("\tChip Mode:   \t%d\n", s2.chip_mode);
    printf("\tReset Status:\t%d\n", s2.reset_status);
    if (!s2.is_running_from_flash) {
        printf("Failure: LR11XX booted back into bootloader mode.\n");
        printf("This means that the LR11XX couldn't verify the firmware flushed or some other error occoured.\n");
        printf("You may try to rerun this firmware updater after a powercycle of the LR11XX.\n");
        lr11xx_close_hal(ctx);
        return 10;
    }
    printf("LR11XX booted back sussccessfully to running onboard flashed firmware.\n");

    // free resouces
    lr11xx_close_hal(ctx);

    printf("LR11XX firmware update susscessful!\n");
    return 0;
}