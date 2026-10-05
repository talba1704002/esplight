idf_component_register(
    SRCS "app_main.cpp"
         "relay_scheduler.cpp"
         "rtc_ds3231.cpp"
         "matter_sync.cpp"
         "web_server.cpp"
         "button_handler.cpp"
         "ap_manager.cpp"
    PRIV_INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html")

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
