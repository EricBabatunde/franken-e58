idf_component_register(
    SRCS "hal_mcpwm.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_config esp_driver_mcpwm
    PRIV_REQUIRES hal soc esp_hw_support freertos
    LDFRAGMENTS "linker.lf")

# M7b, D1b: every panic passes through __wrap_esp_panic_handler (hal_mcpwm.cpp), which forces the
# gates low before the IDF handler prints and reboots. Same mechanism as components/bt in IDF 5.5.3.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
