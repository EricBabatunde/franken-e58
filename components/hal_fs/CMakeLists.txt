idf_component_register(
    SRCS "hal_fs.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_config
    PRIV_REQUIRES joltwallet__littlefs)
