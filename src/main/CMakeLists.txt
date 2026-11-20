idf_component_register(SRCS "kira-lilygo.cpp" INCLUDE_DIRS "." REQUIRES spi_flash platform espressif__elf_loader)
# The generated table is consumed by the platform archive later in the IDF link.
target_link_options(${COMPONENT_LIB} INTERFACE "LINKER:-u,app_exports")
