# SPDX-License-Identifier: Unlicense
# `main` auto-depends on every component (including esp_matter); do not add a
# REQUIRES list here or that auto-dependency turns off.
idf_component_register(SRCS "app_main.cpp"
                            "cc1101.c" "somfy_rts.c" "somfy_frame.c" "blind_store.c"
                       PRIV_INCLUDE_DIRS ".")

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
