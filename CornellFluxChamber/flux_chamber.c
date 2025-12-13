/*
* flux_chamber.c
* 
* When flashed, the Raspberry Pi Pico starts in a sleep cycle, then wakes up
* to do its first measurements. 
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

// sleep
#include "pico/sleep.h"

// watchdog
#include "hardware/watchdog.h"

// ==================================================
// === helper functions
// ==================================================
// Function for wake up alarm
static bool awake;
static void alarm_sleep_callback(uint alarm_id) {
    uart_default_tx_wait_blocking();
    awake = true;
    hardware_alarm_set_callback(alarm_id, NULL);
    hardware_alarm_unclaim(alarm_id);
}

// Function to execute deep sleep
void deep_sleep(uint32_t sleep_time_ms, bool debug_mode) {
    if (debug_mode) {
        printf("Sleeping for %i min %.2f sec\n", (int)sleep_time_ms / 60000, 
            (sleep_time_ms % 60000) / 1000.0);
    }

    // Wait for the fifo to be drained so we get reliable output
    uart_default_tx_wait_blocking();
    // Switch to XOSC to save power
    // Set the crystal oscillator as the dormant clock source, UART will be 
    // reconfigured from here. This is only really necessary before sending 
    // the pico dormant but running from xosc while asleep saves power. 
    sleep_run_from_xosc();
    awake = false;
    // Go to sleep until the alarm interrupt is generated after __ seconds
    uart_default_tx_wait_blocking();
    if (sleep_goto_sleep_for(sleep_time_ms, &alarm_sleep_callback)) {}
    sleep_power_up();   // Re-enabling clock sources and generators.
}

// Function to execute deep sleep, while feeding watchdog every 7000 ms
void watchdog_deep_sleep(uint32_t sleep_time_ms, bool debug_mode) {
    uint32_t remaining_time_ms = sleep_time_ms;

    if (debug_mode) {
        printf("Sleeping for %.2f sec\n", (sleep_time_ms % 60000) / 1000.0);
    }

    while (remaining_time_ms > 0) {
        // maximum chunk size of 7000 ms 
        uint32_t chunk_time_ms = (remaining_time_ms > 7000) ? 7000 : 
        remaining_time_ms;

        uart_default_tx_wait_blocking();
        sleep_run_from_xosc();
        awake = false;
        uart_default_tx_wait_blocking();
        if (sleep_goto_sleep_for(chunk_time_ms, &alarm_sleep_callback)) {}
        sleep_power_up();

        watchdog_update();
        remaining_time_ms -= chunk_time_ms;
    }
}

// Function to move motor - 0 is cw up, 1 is ccw down
// returns elapsed time
uint32_t move_motor(int direction, bool debug_mode) {  
    if (debug_mode) {
        if (direction) { printf("Moving motor down...\n"); } 
        else { printf("Moving motor up...\n"); }
    }

    uint32_t start_us = time_us_32();

    gpio_put(18, 0);            // enable motor
    gpio_put(16, direction);
    // run until reach switch or time limit (of 1 min)
    while ((gpio_get(20 + direction) == !direction)
    && (time_us_32() - start_us) / 1000 < 60000) {
        gpio_put(17, true);
        sleep_us(300);
        gpio_put(17, false);
        sleep_us(300);
    }
    gpio_put(18, 1);            // disable motor
 
    if (gpio_get(20 + direction) == !direction) {
        if (direction) { 
            printf("ERROR: Insufficient power. Failed to close chamber.\n"); } 
        else { printf("ERROR: Insufficient power. Failed to open chamber.\n"); }
    }

    return (time_us_32() - start_us) / 1000;  // elapsed time
}

// Function to recover SCD30 sensor, while feeding watchdog
void recover_scd30(int16_t error, bool debug_mode) {
    // Periodically feed watchdog
    watchdog_update();

    // Powercycle sensor
    gpio_put(11, 0);
    sleep_ms(250);
    gpio_put(11, 1);

    // Reinit sensor
    // make sure the sensor is in a defined state 
    // (soft reset does not stop periodic measurement)
    scd30_stop_periodic_measurement();
    scd30_soft_reset();
    sensirion_i2c_hal_sleep_usec(2000000);
    uint8_t major = 0;
    uint8_t minor = 0;
    error = scd30_read_firmware_version(&major, &minor);
    if (error != NO_ERROR) {
        printf("Error executing read_firmware_version(): %i\n", error);
    }
    if (debug_mode) {
        printf("firmware version major: %u minor: %u\n", major, minor);
    }
    // The 0 parameter disables ambient pressure compensation (can be replaced 
    // with actual pressure value in mBar if needed).
    error = scd30_start_periodic_measurement(0);
    if (error != NO_ERROR) {
        printf("Error executing start_periodic_measurement(): %i\n", error);
    }    
}

// ===========================================
// === chamber thread on core 1
// ===========================================
static PT_THREAD(protothread_chamber(struct pt *pt))
{
    PT_BEGIN(pt);

    // ==================================================
    // === CUSTOMIZABLE PARAMETERS
    // ==================================================
    // Filename (specify format as .txt or .csv)
    static char filename[64] = "test.csv";

    // Date and time
    int year = 2026, month = 01, day = 01;  // yyyy-m-d
    int hour = 0, min = 0, sec = 0;      // h:m:s

    // Total time to log data each cycle [ms]
    static uint32_t total_sampling_ms = 900000; // 15 min

    // Time between each data measurement entry [ms]
    // required: must be <= total_sampling_ms
    static uint32_t sampling_interval_ms = 5000; // 5 s

    // Total time to flush the chamber air each cycle [ms]
    // The canopy will open at the start of the time and close at the end
    // required: must be >= 2 x time to open/close chamber
    static uint32_t flush_chamber_ms = 600000;  // 10 min
    
    // Debug mode provides print statements to the serial monitor
    // Error messages still print without debug mode 
    static bool debug_mode = false;

    // ==================================================
    // === initializations
    // ==================================================
    static bool first = true;

    // Initialize date & time
    char datetime_buf[256];
    char *datetime_str = &datetime_buf[0];
    rtc_init();
    datetime_t t = {
        .year = year,
        .month = month,
        .day = day,
        .hour = hour,
        .min = min,
        .sec = sec};
    rtc_set_datetime(&t);

    // Initialize SD card
    if (!sd_init_driver()) {
        printf("ERROR: Could not initialize SD card\r\n");
        while (1);
    }

    // Mount drive
    fr = f_mount(&fs, "0:", 1);
    if (fr != FR_OK) {
        printf("ERROR: Could not mount filesystem (%d)\r\n", fr);
        while (1);
    }

    // Initialize KN3904 (NPN) transistor for power
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
    if (error != NO_ERROR) {
        printf("Error executing read_firmware_version(): %i\n", error);
    }
    if (debug_mode) {
        printf("Firmware version major: %u minor: %u\n", major, minor);
    }
    // The 0 parameter disables ambient pressure compensation (can be replaced 
    // with actual pressure value in mBar if needed).
    error = scd30_start_periodic_measurement(0);
    if (error != NO_ERROR) {
        printf("Error executing start_periodic_measurement(): %i\n", error);
    }

    // Initialize ADC... for Methane Sensor
    adc_init();
    adc_gpio_init(26);      // Make sure GPIO is high-impedance, no pullups etc
    adc_select_input(0);    // Select ADC input 0 (GPIO26)

    // Initialize Motor Pins
    uint32_t elapsed_time_ms = 0; 
    uint32_t sleep_time_ms = 0;
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

    while (1) {
        // ===========================================
        // === air exchange
        // ===========================================
        // Open chamber
        elapsed_time_ms = move_motor(0, debug_mode); 

        // Sleep
        if (elapsed_time_ms < flush_chamber_ms) {
            sleep_time_ms = flush_chamber_ms - 2 * elapsed_time_ms;
            deep_sleep(sleep_time_ms, debug_mode);
        }

        // Close chamber
        elapsed_time_ms = move_motor(1, debug_mode);

        // ===========================================
        // === log data
        // ===========================================
        uint32_t start_us = time_us_32();

        FIL f_dst;
        pico_unique_board_id_t PICO_ID;

        if (FR_OK != f_open(&f_dst, filename, FA_WRITE | FA_OPEN_APPEND)) {
            printf("Cannot create '%s'\r\n", filename);
        }
        else {
            UINT wr_count = 0;
            const char *NEW_LINE = "\n";

            // write the header once
            if (first) {
                first = false; 

                // Write UTF-8 BOM (for excel to interpret special characters)
                BYTE bom[] = {0xEF, 0xBB, 0xBF};
                f_write(&f_dst, bom, sizeof(bom), &wr_count);

                // Write pico id
                pico_get_unique_board_id(&PICO_ID);
                // Buffer for hexadecimal string
                char id_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2]; 
                for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
                    // Append each byte in hexadecimal format to the buffer
                    sprintf(&id_str[i * 2], "%02X", PICO_ID.id[i]);
                }
                if (debug_mode) {
                    printf("ID is: %s \n", id_str);
                }
                f_write(&f_dst, "Pico ID,", strlen("Pico ID,"), &wr_count);
                f_write(&f_dst, id_str, strlen(id_str), &wr_count);
                f_write(&f_dst, NEW_LINE, strlen(NEW_LINE), &wr_count);

                // Write header
                const char *DATA_HEADER = 
                "DATETIME,TEMP (°C),HUM (%RH),CO2 (ppm),CH4\n";
                f_write(&f_dst, DATA_HEADER, strlen(DATA_HEADER), &wr_count);
            }

            // Write contents

            // Write if motor was not supplied sufficient power
            if (elapsed_time_ms >= 60000) {
                f_write(&f_dst, "ERROR: Insufficient power to motor,", 
                    strlen("ERROR: Insufficient power to motor,"), &wr_count);
                f_write(&f_dst, NEW_LINE, strlen(NEW_LINE), &wr_count);
            }

            // Enable watchdog for maximum 8000 ms
            // 2nd arg to pause on debug mode
            watchdog_enable(8000, 1);

            while (((time_us_32() - start_us) / 1000) + sampling_interval_ms < 
            total_sampling_ms) {
                watchdog_update();
                
                // Sleep between writes
                watchdog_deep_sleep(sampling_interval_ms, debug_mode);

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
                error = scd30_blocking_read_measurement_data(&co2,&temp, &hum);
                if (error != NO_ERROR) {
                    printf("Error executing blocking_read_measurement_data(): "
                        "%i\n", error);

                    // Create new line in write file
                    f_write(&f_dst, NEW_LINE, strlen(NEW_LINE), &wr_count);
                    f_sync(&f_dst);

                    recover_scd30(error, debug_mode);
                    
                    continue;
                }

                char temp_str[10] = {0};
                sprintf(temp_str, "%f", temp);
                f_write(&f_dst, temp_str, strlen(temp_str), &wr_count);
                f_write(&f_dst, ",", strlen(","), &wr_count);

                char hum_str[10] = {0};
                sprintf(hum_str, "%f", hum);
                f_write(&f_dst, hum_str, strlen(hum_str), &wr_count);
                f_write(&f_dst, ",", strlen(","), &wr_count);

                char co2_str[10] = {0};
                sprintf(co2_str, "%f", co2);
                f_write(&f_dst, co2_str, strlen(co2_str), &wr_count);
                f_write(&f_dst, ",", strlen(","), &wr_count);
                
                // Get & write methane concentration
                uint16_t methane = adc_read();
                char methane_str[10] = {0};
                sprintf(methane_str, "%d", methane);
                f_write(&f_dst, methane_str, strlen(methane_str), &wr_count);
                
                // Create new line
                f_write(&f_dst, NEW_LINE, strlen(NEW_LINE), &wr_count);

                // Print in debug mode
                if (debug_mode) {
                    printf("DATETIME: %s", datetime_str);
                    printf(", TEMP: %f", temp);
                    printf(", HUM: %f", hum);
                    printf(", CO2: %f", co2);
                    printf(", CH4: %d\n", methane);
                }
                
                // Write cached information periodically
                f_sync(&f_dst);
            }
            watchdog_disable();
            f_close(&f_dst);
        }

        // Sleep until reach the full total_sampling_ms
        if (((time_us_32() - start_us) / 1000) < total_sampling_ms) {
            deep_sleep(total_sampling_ms - ((time_us_32() - start_us) / 1000), 
            debug_mode);
        }
    }
    PT_END(pt);
} // chamber thread

// ========================================
// === core 1 main -- started in main below
// ========================================
void core1_main()
{
    //  === add threads  ====================
    // for core 1
    pt_add_thread(protothread_chamber); // needs to run on core 1!!
    //
    // === initialize the scheduler ==========
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

    // === initalize the scheduler ===============
    pt_sched_method = SCHED_PRIORITY;
    pt_schedule_start;
    // NEVER exits
    // ===========================================
} // end main