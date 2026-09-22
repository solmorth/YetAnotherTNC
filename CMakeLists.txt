cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(peripheral_nus)

target_sources(app PRIVATE
        src/main.c
        src/mode_select.c
        src/ptt.c
        src/gps.c
        src/gps_format.c
        src/tnc_config.c
        src/ax25.c
        src/fx25.c
        src/kiss.c
        src/tnc.c
        src/afsk_modulator.cpp
        src/afsk_demodulator.cpp
        src/audio_tx_pwm.cpp
        src/audio_rx_adc.cpp
)
