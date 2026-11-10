#
# Copyright (c) 2026 Toshimiyu 
#
# Released under the MIT license
# https://opensource.org/licenses/mit-license.php
#

# Select the required CMSIS-DSP source files (keep the list minimal for the functions in use).
file(GLOB DSP_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/../CMSIS/DSP/Source/TransformFunctions/arm_cfft_f32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../CMSIS/DSP/Source/TransformFunctions/arm_cfft_init_f32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../CMSIS/DSP/Source/TransformFunctions/arm_cfft_radix8_f32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../CMSIS/DSP/Source/TransformFunctions/arm_rfft_fast_f32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../CMSIS/DSP/Source/TransformFunctions/arm_rfft_fast_init_f32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../CMSIS/DSP/Source/TransformFunctions/arm_bitreversal.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../CMSIS/DSP/Source/TransformFunctions/arm_bitreversal2.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../CMSIS/DSP/Source/CommonTables/arm_common_tables.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../CMSIS/DSP/Source/CommonTables/arm_const_structs.c
        )

# Add executable. Default name is the project name, version 0.1
set(USB_AUDIO_CLASS_VERSION "2" CACHE STRING "USB Audio Class version (1 or 2)")
set_property(CACHE USB_AUDIO_CLASS_VERSION PROPERTY STRINGS 1 2)
if(NOT USB_AUDIO_CLASS_VERSION STREQUAL "1" AND NOT USB_AUDIO_CLASS_VERSION STREQUAL "2")
        message(FATAL_ERROR "USB_AUDIO_CLASS_VERSION must be 1 or 2")
endif()

set(USB_AUDIO_PREFILL_FRAMES "384" CACHE STRING "Legacy drain start threshold in stereo frames")
if(NOT USB_AUDIO_PREFILL_FRAMES MATCHES "^[0-9]+$"
        OR USB_AUDIO_PREFILL_FRAMES LESS 1
        OR USB_AUDIO_PREFILL_FRAMES GREATER_EQUAL 1536)
        message(FATAL_ERROR "USB_AUDIO_PREFILL_FRAMES must be an integer from 1 to 1535")
endif()

set(PICO_FIRMWARE_VERSION "1.0")

if(USB_AUDIO_CLASS_VERSION STREQUAL "1")
        set(USB_AUDIO_CLASS_SOURCE usb_audio_uac1.c)
else()
        set(USB_AUDIO_CLASS_SOURCE usb_audio_uac2.c)
endif()

add_executable(Pico2_UAC12_USBDDC_Weather
        main.c
        general_func.c
        usb_device_control.c
        ${USB_AUDIO_CLASS_SOURCE}
        usb_descriptors.c
        cdc_diagnostics.c
        weather_rx.c
        core1.c
        i2s_pio_interface.c
        transmit_to_dac.c
        ringbuffer.c
        debug_gauge.c
        display_rta.c
        spectrum.c
        ${DSP_SRC}
)

set(PICO_FIRMWARE_NAME "Pico2_UAC12_USBDDC_Weather_UAC${USB_AUDIO_CLASS_VERSION}")
set_target_properties(Pico2_UAC12_USBDDC_Weather PROPERTIES OUTPUT_NAME "${PICO_FIRMWARE_NAME}")

# UART console is unused; do not pull pico_stdio_uart/hardware_uart into the image.
pico_enable_stdio_uart(Pico2_UAC12_USBDDC_Weather 0)

# Copy the program to SRAM and execute it at startup (RP2350).
pico_set_binary_type(Pico2_UAC12_USBDDC_Weather copy_to_ram)

# Reserve the top 64 KiB SRAM bank for the I2S DMA source buffers.
pico_set_linker_script_var(Pico2_UAC12_USBDDC_Weather RAM_LENGTH 0x70000)
pico_add_linker_script_override_path(Pico2_UAC12_USBDDC_Weather
        ${CMAKE_CURRENT_LIST_DIR}/linker
        FILES memory_extra.incl section_extra_post_data.incl
)

pico_set_program_name(Pico2_UAC12_USBDDC_Weather "${PICO_FIRMWARE_NAME}")
pico_set_program_version(Pico2_UAC12_USBDDC_Weather "${PICO_FIRMWARE_VERSION}")

# Generate PIO header
pico_generate_pio_header(Pico2_UAC12_USBDDC_Weather ${CMAKE_CURRENT_LIST_DIR}/i2s.pio)

# Add the standard library to the build
target_link_libraries(Pico2_UAC12_USBDDC_Weather
        pico_stdlib)

# Add the standard include files to the build
target_include_directories(Pico2_UAC12_USBDDC_Weather PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../pico-extras
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_LIST_DIR}/../CMSIS/Core/Include
        ${CMAKE_CURRENT_LIST_DIR}/../CMSIS/DSP/Include
        )

target_compile_definitions(Pico2_UAC12_USBDDC_Weather PRIVATE
        USB_AUDIO_CLASS_VERSION=${USB_AUDIO_CLASS_VERSION}
        USB_AUDIO_PREFILL_FRAMES=${USB_AUDIO_PREFILL_FRAMES}
        FW_VERSION="${PICO_FIRMWARE_VERSION}"
        )

# Add any user requested libraries
target_link_libraries(Pico2_UAC12_USBDDC_Weather
        hardware_pio 
        tinyusb_device
        pico_multicore
        hardware_clocks
        hardware_dma
        hardware_pwm
        pico_float
        hardware_spi
        hardware_adc
        pico_unique_id
)

pico_set_float_implementation(Pico2_UAC12_USBDDC_Weather pico_vfp)

target_link_options(Pico2_UAC12_USBDDC_Weather PRIVATE
        "LINKER:-u,__wrap_printf"
        "LINKER:-u,__wrap_puts"
        "LINKER:-u,__wrap_putchar"
        "LINKER:-u,__wrap_getchar"
        "LINKER:-u,__wrap_vprintf"
        "LINKER:-u,__wrap_powf"
        "LINKER:-u,__wrap_log10f"
)

pico_add_extra_outputs(Pico2_UAC12_USBDDC_Weather)
