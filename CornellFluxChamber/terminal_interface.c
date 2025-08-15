/*
 * terminal_interface.c
 *
 * Works like your computer's terminal or PowerShell. Start with 'help' to find
 * the valid commands.
 */

/*
 FatFs license
 Copyright 2021 Carl John Kugler III

Licensed under the Apache License, Version 2.0 (the License); you may not use
this file except in compliance with the License. You may obtain a copy of the
License at

   http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an AS IS BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.
*/
/*
 * This code borrows heavily from the Mbed SDBlockDevice:
 *       https://os.mbed.com/docs/mbed-os/v5.15/apis/sdblockdevice.html
 *       mbed-os/components/storage/blockdevice/COMPONENT_SD/SDBlockDevice.cpp
 *
 * Editor: Carl Kugler (carlk3@gmail.com)
 *
 * Remember your ABCs: "Always Be Cobbling!"
 *
 */

/*TINYUSB license (for the CLI commnads)
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

/* mbed Microcontroller Library
 * Copyright (c) 2006-2013 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Introduction
 * ------------
 * SD and MMC cards support a number of interfaces, but common to them all
 * is one based on SPI. Since we already have the mbed SPI Interface, it will
 * be used for SD cards.
 *
 * The main reference I'm using is Chapter 7, "SPI Mode" of:
 *  http://www.sdcard.org/developers/tech/sdcard/pls/Simplified_Physical_Layer_Spec.pdf
 *
 * SPI Startup
 * -----------
 * The SD card powers up in SD mode. The start-up procedure is complicated
 * by the requirement to support older SDCards in a backwards compatible
 * way with the new higher capacity variants SDHC and SDHC.
 *
 * The following figures from the specification with associated text describe
 * the SPI mode initialisation process:
 *  - Figure 7-1: SD Memory Card State Diagram (SPI mode)
 *  - Figure 7-2: SPI Mode Initialization Flow
 *
 * Firstly, a low initial clock should be selected (in the range of 100-
 * 400kHZ). After initialisation has been completed, the switch to a
 * higher clock speed can be made (e.g. 1MHz). Newer cards will support
 * higher speeds than the default _transfer_sck defined here.
 *
 * Next, note the following from the SDCard specification (note to
 * Figure 7-1):
 *
 *  In any of the cases CMD1 is not recommended because it may be difficult for
 * the host to distinguish between MultiMediaCard and SD Memory Card
 *
 * Hence CMD1 is not used for the initialisation sequence.
 *
 * The SPI interface mode is selected by asserting CS low and sending the
 * reset command (CMD0). The card will respond with a (R1) response.
 * In practice many cards initially respond with 0xff or invalid data
 * which is ignored. Data is read until a valid response is received
 * or the number of re-reads has exceeded a maximim count. If a valid
 * response is not received then the CMD0 can be retried. This
 * has been found to successfully initialise cards where the SPI master
 * (on MCU) has been reset but the SDCard has not, so the first
 * CMD0 may be lost.
 *
 * CMD8 is optionally sent to determine the voltage range supported, and
 * indirectly determine whether it is a version 1.x SD/non-SD card or
 * version 2.x. I'll just ignore this for now.
 *
 * ACMD41 is repeatedly issued to initialise the card, until "in idle"
 * (bit 0) of the R1 response goes to '0', indicating it is initialised.
 *
 * You should also indicate whether the host supports High Capicity cards,
 * and check whether the card is high capacity - i'll also ignore this.
 *
 * SPI Protocol
 * ------------
 * The SD SPI protocol is based on transactions made up of 8-bit words, with
 * the host starting every bus transaction by asserting the CS signal low. The
 * card always responds to commands, data blocks and errors.
 *
 * The protocol supports a CRC, but by default it is off (except for the
 * first reset CMD0, where the CRC can just be pre-calculated, and CMD8)
 * I'll leave the CRC off I think!
 *
 * Standard capacity cards have variable data block sizes, whereas High
 * Capacity cards fix the size of data block to 512 bytes. I'll therefore
 * just always use the Standard Capacity cards with a block size of 512 bytes.
 * This is set with CMD16.
 *
 * You can read and write single blocks (CMD17, CMD25) or multiple blocks
 * (CMD18, CMD25). For simplicity, I'll just use single block accesses. When
 * the card gets a read command, it responds with a response token, and then
 * a data token or an error.
 *
 * SPI Command Format
 * ------------------
 * Commands are 6-bytes long, containing the command, 32-bit argument, and CRC.
 *
 * +---------------+------------+------------+-----------+----------+--------------+
 * | 01 | cmd[5:0] | arg[31:24] | arg[23:16] | arg[15:8] | arg[7:0] | crc[6:0] |
 * 1 |
 * +---------------+------------+------------+-----------+----------+--------------+
 *
 * As I'm not using CRC, I can fix that byte to what is needed for CMD0 (0x95)
 *
 * All Application Specific commands shall be preceded with APP_CMD (CMD55).
 *
 * SPI Response Format
 * -------------------
 * The main response format (R1) is a status byte (normally zero). Key flags:
 *  idle - 1 if the card is in an idle state/initialising
 *  cmd  - 1 if an illegal command code was detected
 *
 *    +-------------------------------------------------+
 * R1 | 0 | arg | addr | seq | crc | cmd | erase | idle |
 *    +-------------------------------------------------+
 *
 * R1b is the same, except it is followed by a busy signal (zeros) until
 * the first non-zero byte when it is ready again.
 *
 * Data Response Token
 * -------------------
 * Every data block written to the card is acknowledged by a byte
 * response token
 *
 * +----------------------+
 * | xxx | 0 | status | 1 |
 * +----------------------+
 *              010 - OK!
 *              101 - CRC Error
 *              110 - Write Error
 *
 * Single Block Read and Write
 * ---------------------------
 *
 * Block transfers have a byte header, followed by the data, followed
 * by a 16-bit CRC. In our case, the data will always be 512 bytes.
 *
 * +------+---------+---------+- -  - -+---------+-----------+----------+
 * | 0xFE | data[0] | data[1] |        | data[n] | crc[15:8] | crc[7:0] |
 * +------+---------+---------+- -  - -+---------+-----------+----------+
 */

/*
 * Copyright (c) 2022, Sensirion AG
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Sensirion AG nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

// ==================================================
// === initializations
// ==================================================
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

//  FatFs sd card
#include "sd_card.h" // FatFs_SPI/sd_driver/
#include "ff.h"      // FatFs_SPI/ff14a/source/
FRESULT fr;
FATFS fs;

// protothreads
#include "pico/multicore.h"
#include "pt_cornell_rp2040_v1_3.h"

// data logging
#include "pico/unique_id.h"
#include "hardware/rtc.h"

// SCD30 co2 sensor
#include "scd30_i2c.h"
#include "sensirion_common.h"
#include "sensirion_i2c_hal.h"

// methane sensor
#include "hardware/adc.h"

// ===========================================
// === serial and file i/o thread on core 1
// ===========================================
// The command interpreter is largely copied from the tinyusb disribution.
// https://github.com/hathach/tinyusb/blob/master/examples/host/msc_file_explorer/src/main.c
// See license at top of file
// ===========================================
static PT_THREAD(protothread_chamber(struct pt *pt))
{
    PT_BEGIN(pt);
    static char cmd[16], arg1[64], arg2[16], arg3[16];
    static char *token;

    // Default datetime set to Sunday 01 January 00:00:00 2000
    char datetime_buf[256];
    char *datetime_str = &datetime_buf[0];
    rtc_init();
    datetime_t t = {
        .year = 2000,
        .month = 01,
        .day = 01,
        .hour = 00,
        .min = 00,
        .sec = 00};
    rtc_set_datetime(&t);

    // Default time between each data measurement entry [ms] -- 5 seconds
    static int logging_period_ms = 5000;

    // Initialize SD card
    if (!sd_init_driver())
    {
        printf("ERROR: Could not initialize SD card\r\n");
        while (true)
            ;
    }

    // Mount drive
    fr = f_mount(&fs, "0:", 1);
    if (fr != FR_OK)
    {
        printf("ERROR: Could not mount filesystem (%d)\r\n", fr);
        while (true)
            ;
    }

    // Initialize KN3904 (npn) transistor for power
    gpio_init(11);
    gpio_set_dir(11, GPIO_OUT);
    gpio_put(11, 1);        // turn on power to SCD30
    // Initialize SCD30 Sensor
    float co2 = 0.0;
    float temp = 0.0;
    float hum = 0.0;
    int16_t error = NO_ERROR;
    sensirion_i2c_hal_init();
    init_driver(SCD30_I2C_ADDR_61);
    // make sure the sensor is in a defined state
    // (soft reset does not stop periodic measurement)
    scd30_stop_periodic_measurement();
    scd30_soft_reset();
    sensirion_i2c_hal_sleep_usec(2000000);
    uint8_t major = 0;
    uint8_t minor = 0;
    error = scd30_read_firmware_version(&major, &minor);
    if (error != NO_ERROR)
    {
        printf("error executing read_firmware_version(): %i\n", error);
        return error;
    }
    printf("firmware version major: %u minor: %u\n", major, minor);
    // The 0 parameter disables ambient pressure compensation (can be replaced 
    // with actual pressure value in mBar if needed).
    error = scd30_start_periodic_measurement(0);
    if (error != NO_ERROR)
    {
        printf("error executing start_periodic_measurement(): %i\n", error);
        return error;
    }

    // Initialize ADC... for Methane Sensor
    adc_init();
    adc_gpio_init(26);   // Make sure GPIO is high-impedance, no pullups etc
    adc_select_input(0); // Select ADC input 0 (GPIO26)

    // Initialize Motor Pins
    gpio_init(18);              // enable pin
    gpio_set_dir(18, GPIO_OUT);
    gpio_init(17);              // step pin
    gpio_set_dir(17, GPIO_OUT);
    gpio_init(16);              // direction pin
    gpio_set_dir(16, GPIO_OUT);
    gpio_put(18, 1);            // disable motor when not in use

    // Initialize Switch Pins
    gpio_init(20);              // limit switch
    gpio_set_dir(20, GPIO_IN);
    gpio_pull_up(20);
    gpio_init(21);              // float switch
    gpio_set_dir(21, GPIO_IN);
    gpio_pull_up(21);

    printf("Default date: Sunday 01 January 00:00:00 2000\n");
    printf("Default period: 5 seconds\n");
    printf("Enter 'help' to find the valid commands\n");

    // main loop of terminal interface
    while (1)
    {
        printf(">>");
        // spawn a thread to do the non-blocking serial read
        serial_read;
        // tokenize serial input
        token = strtok(pt_serial_in_buffer, "  ");
        strcpy(cmd, token);
        token = strtok(NULL, "  ");
        strcpy(arg1, token);
        token = strtok(NULL, "  ");
        strcpy(arg2, token);
        token = strtok(NULL, "  ");
        strcpy(arg3, token);

        // different cases for different commands
        // ===
        if (strcmp(cmd, "help") == 0)
        {
            printf("*****\n\r");
            printf("[1] test_motor <direction> -- run the motor up/down "
                "for a certain number of steps \n\r");
            printf("[2] set_date <yyyy-mm-dd> <hh:mm:ss> -- set the RTC "
                "intialization of date/time\n\r");
            printf("[3] set_per <period> -- set the data logging period in ms\n\r");
            printf("[4] write <filename> -- write the Pico unique ID, date/time"
                ", and data to file saved as .txt or .csv (specify format in "
                "filename)\n\r");
            printf("[5] print <filename> -- print the content of the filename"
                "\n\r");
            printf("[6] rm <filename> -- delinks (deletes) a file\n\r");
            printf("[7] ls <directory> -- list directory contents\n\r");
            printf("[8] cd <directory> -- changes current directory\n\r");
            printf("[9] mkdir <directory> -- new directory\n\r");
            printf("*****\n\r");
        }
        
        // ===
        if (strcmp(cmd, "test_motor") == 0)
        {
            char dir_str[10];
            int steps;

            if ((sscanf(arg1, "%s", dir_str) == 1) && ((strcmp(dir_str, "up") == 0) || (strcmp(dir_str, "down") == 0)))
            {
                int direction; 
                direction = (strcmp(dir_str, "up") == 0) ? 0 : 1;
                if (direction) 
                {
                    printf("Moving motor down. Raise the float to stop.\n");
                }
                else
                {
                    printf("Moving motor up. Press the limit switch to stop.\n");

                }
              
                gpio_put(18, 0);            // enable motor
                gpio_put(16, direction);
                while (gpio_get(20 + direction) == !direction) {
                    gpio_put(17, true);
                    sleep_us(1000);
                    gpio_put(17, false);
                    sleep_us(1000);
                }
                gpio_put(18, 1);            // disable motor
            }
            else
            {
                printf("Invalid direction. Please use 'up' or 'down'.\n");
            }
        }

        // ===
        if (strcmp(cmd, "set_date") == 0)
        {
            static char year[16], month[16], day[16];
            static char hour[16], minute[16], second[16];
            static char *datetoken, *timetoken;
            char date, time;

            // tokenize serial input
            if (sscanf(arg1, "%s", &date) == 1 && sscanf(arg2, "%s", &time) == 1)
            {
                datetoken = strtok(&date, "-");
                strcpy(year, datetoken);
                datetoken = strtok(NULL, "-");
                strcpy(month, datetoken);
                datetoken = strtok(NULL, "-");
                strcpy(day, datetoken);

                timetoken = strtok(&time, ":");
                strcpy(hour, timetoken);
                timetoken = strtok(NULL, ":");
                strcpy(minute, timetoken);
                timetoken = strtok(NULL, ":");
                strcpy(second, timetoken);

                printf("Setting RTC to %s-%s-%s %s:%s:%s\n\r", &year, &month, 
                    &day, &hour, &minute, &second);

                rtc_init();
                datetime_t t = {
                    .year = atoi(year),
                    .month = atoi(month),
                    .day = atoi(day),
                    .hour = atoi(hour),
                    .min = atoi(minute),
                    .sec = atoi(second)};
                rtc_set_datetime(&t);
            }
            else
            {
                printf("Invalid date format: %s %s\n\r", arg1, arg2);
            }
        }

        // ===
        if (strcmp(cmd, "set_per") == 0)
        {
            int per;
            if (sscanf(arg1, "%d", &per) == 1 && per > 0)
            {
                logging_period_ms = per;
                printf("Logging period set to %d ms\n", logging_period_ms);
            }
            else
            {
                printf("Invalid period. Please enter a positive integer.\n");
            }
        }

        // ===
        if (strcmp(cmd, "write") == 0)
        {
            FIL f_dst;
            pico_unique_board_id_t PICO_ID;

            if (FR_OK != f_open(&f_dst, arg1, FA_WRITE | FA_OPEN_APPEND))
            {
                printf("Cannot create '%s'\r\n", arg1);
            }
            else
            {
                UINT wr_count = 0;
                char *NEW_LINE = "\n";

                // Write UTF-8 BOM (for excel to interpret special characters)
                BYTE bom[] = {0xEF, 0xBB, 0xBF};
                f_write(&f_dst, bom, sizeof(bom), &wr_count);

                // Write pico id
                pico_get_unique_board_id(&PICO_ID);
                // Buffer for hexadecimal string
                char id_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2]; 
                for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++)
                {
                    // Append each byte in hexadecimal format to the buffer
                    sprintf(&id_str[i * 2], "%02X", PICO_ID.id[i]);
                }
                printf("ID is: %s \n", id_str);
                f_write(&f_dst, "Pico ID,", strlen("Pico ID,"), &wr_count);
                f_write(&f_dst, id_str, strlen(id_str), &wr_count);
                f_write(&f_dst, NEW_LINE, strlen(NEW_LINE), &wr_count);

                // Write header
                const char *DATA_HEADER = "DATETIME,TEMP (°C),HUM (%RH),CO2 (ppm),CH4\n";
                f_write(&f_dst, DATA_HEADER, strlen(DATA_HEADER), &wr_count);

                // Write contents
                for (int i = 0; i < 5; i++) 
                {
                    // Sleep between writes
                    sleep_ms(logging_period_ms);

                    // Get & write date/time
                    rtc_get_datetime(&t);
                    char datetime_str[20] = {0};
                    snprintf(datetime_str, sizeof(datetime_str),
                            "%02d/%02d/%02d %02d:%02d:%02d",
                            t.month, t.day, t.year % 100,
                            t.hour, t.min, t.sec);
                    f_write(&f_dst, datetime_str, strlen(datetime_str), &wr_count);
                    f_write(&f_dst, ",", strlen(","), &wr_count);

                    // Get & write temperature, humidity, co2 concentration
                    error = scd30_blocking_read_measurement_data(&co2, &temp, &hum);
                    if (error != NO_ERROR)
                    {
                        printf("Error executing blocking_read_measurement_data"
                            "(): %i\n", error);
                        continue;
                    }

                    printf("TEMP: %f", temp);
                    char temp_str[10] = {0};
                    sprintf(temp_str, "%f", temp);
                    f_write(&f_dst, temp_str, strlen(temp_str), &wr_count);
                    f_write(&f_dst, ",", strlen(","), &wr_count);

                    printf(", HUM: %f", hum);
                    char hum_str[10] = {0};
                    sprintf(hum_str, "%f", hum);
                    f_write(&f_dst, hum_str, strlen(hum_str), &wr_count);
                    f_write(&f_dst, ",", strlen(","), &wr_count);

                    printf(", CO2: %f", co2);
                    char co2_str[10] = {0};
                    sprintf(co2_str, "%f", co2);
                    f_write(&f_dst, co2_str, strlen(co2_str), &wr_count);
                    f_write(&f_dst, ",", strlen(","), &wr_count);

                    // Get & write methane concentration
                    uint16_t methane = adc_read();
                    printf(", CH4: %d\n", methane);
                    char methane_str[10] = {0};
                    sprintf(methane_str, "%d", methane);
                    f_write(&f_dst, methane_str, strlen(methane_str), &wr_count);

                    // Create new line
                    f_write(&f_dst, NEW_LINE, strlen(NEW_LINE), &wr_count);

                    // Write cached information periodically
                    f_sync(&f_dst);
                }
                f_close(&f_dst);
            }
        }

        // ===
        if (strcmp(cmd, "print") == 0)
        {
            FIL fi;
            if (FR_OK != f_open(&fi, arg1, FA_READ))
            {
                printf("%s: No such file or directory\r\n", arg1);
            }
            else
            {
                uint8_t buf[512];
                UINT count = 0;
                while ((FR_OK == f_read(&fi, buf, sizeof(buf), &count)) && 
                (count > 0))
                {
                    for (UINT c = 0; c < count; c++)
                    {
                        const uint8_t ch = buf[c];
                        putchar(ch);
                    }
                }
                printf("\n\r");
            }
            f_close(&fi);
        }

        // ===
        if (strcmp(cmd, "rm") == 0)
        {
            const char *fpath = arg1; // token count from 1
            if (FR_OK != f_unlink(fpath))
            {
                printf("Cannot remove '%s': No such file or directory\r\n", 
                    fpath);
            }
        }

        // ===
        if (strcmp(cmd, "ls") == 0)
        {
            // default is current directory
            const char *dpath = ".";
            if (arg1)
                dpath = arg1;

            DIR dir;
            if (FR_OK != f_opendir(&dir, dpath))
            {
                printf("Cannot access '%s': No such file or directory\r\n", 
                    dpath);
            }

            char path[256];
            if (FR_OK != f_getcwd(path, sizeof(path)))
            {
                printf("Cannot get current working directory\r\n");
            }

            printf("Current directory: %s\n\r", path);

            FILINFO fno;
            while ((f_readdir(&dir, &fno) == FR_OK) && (fno.fname[0] != 0))
            {
                if (fno.fname[0] != '.') // ignore . and .. entry
                {
                    if (fno.fattrib & AM_DIR)
                    {
                        // directory
                        printf("/%s\r\n", fno.fname);
                    }
                    else
                    {
                        printf("%-40s", fno.fname);
                        if (fno.fsize < 1000)
                        {
                            printf("%llu B\r\n", fno.fsize);
                        }
                        else
                        {
                            printf("%llu KB\r\n", fno.fsize / 1000);
                        }
                    }
                }
            }
            f_closedir(&dir);
        }

        // ===
        if (strcmp(cmd, "cd") == 0)
        {
            // default is current directory
            const char *dpath = arg1;

            if (FR_OK != f_chdir(dpath))
            {
                printf("%s: No such file or directory\r\n", dpath);
            }
        }

        // ===
        if (strcmp(cmd, "mkdir") == 0)
        {
            const char *dpath = arg1;
            if (FR_OK != f_mkdir(dpath))
            {
                printf("%s: Cannot create this directory\r\n", dpath);
            }
        }
    }
    PT_END(pt);
} // file thread

// ========================================
// === core 1 main -- started in main below
// ========================================
void core1_main()
{
    //  === add threads  ====================
    // for core 1
    pt_add_thread(protothread_chamber);
    //
    // === initalize the scheduler ==========
    pt_sched_method = SCHED_ROUND_ROBIN;

    pt_schedule_start;
    // NEVER exits
    // ======================================
}

// ========================================
// === core 0 main
// ========================================
int main()
{
    //  start the serial i/o
    stdio_init_all();

    // start core 1 threads
    multicore_reset_core1();
    multicore_launch_core1(&core1_main);

    // === config threads ========================
    // for core 0

    // === initalize the scheduler ===============
    pt_sched_method = SCHED_PRIORITY;
    pt_schedule_start;
    // NEVER exits
    // ===========================================
} // end main