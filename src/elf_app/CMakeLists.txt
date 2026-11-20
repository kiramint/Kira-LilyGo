# Compile with the firmware's public component headers. Only application objects
# are linked into the DSO. Undefined symbols resolve against the running host.
function(t_embed_app id)
    add_library(${id}_objects OBJECT ${ARGN})
    target_include_directories(${id}_objects PRIVATE "$<TARGET_PROPERTY:__idf_platform,INCLUDE_DIRECTORIES>")
    target_compile_definitions(${id}_objects PRIVATE "$<TARGET_PROPERTY:__idf_platform,COMPILE_DEFINITIONS>")
    target_compile_options(${id}_objects PRIVATE -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
    set(dir "${CMAKE_BINARY_DIR}/sdcard/apps/${id}")
    set(image "${dir}/app.elf")
    add_custom_command(OUTPUT "${image}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${dir}"
        COMMAND ${CMAKE_CXX_COMPILER} -nostdlib -shared -Wl,-e,app_main,--gc-sections,-s -o "${image}" $<TARGET_OBJECTS:${id}_objects>
        DEPENDS ${id}_objects $<TARGET_OBJECTS:${id}_objects> COMMAND_EXPAND_LISTS VERBATIM)
    add_custom_target(${id}_image DEPENDS "${image}")
    set_property(GLOBAL APPEND PROPERTY T_EMBED_IMAGE_TARGETS ${id}_image)
    set_property(GLOBAL APPEND PROPERTY T_EMBED_IMAGES "${image}")
    set(manifest "${dir}/manifest.json")
    add_custom_command(OUTPUT "${manifest}" COMMAND ${PYTHON} "${PROJECT_DIR}/tools/package.py"
        manifest "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.elf" "${manifest}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" "${PROJECT_DIR}/tools/package.py" ${CMAKE_PROJECT_NAME}.elf VERBATIM)
    add_custom_target(${id}_package ALL DEPENDS "${manifest}")
endfunction()
add_subdirectory(scope)
add_subdirectory(tone)
get_property(images GLOBAL PROPERTY T_EMBED_IMAGES)
get_property(image_targets GLOBAL PROPERTY T_EMBED_IMAGE_TARGETS)
set(exports "${CMAKE_CURRENT_BINARY_DIR}/exports.c")
add_custom_command(OUTPUT "${exports}" COMMAND ${PYTHON} "${PROJECT_DIR}/tools/package.py" exports ${CMAKE_NM} "${exports}" ${images}
    DEPENDS ${images} "${PROJECT_DIR}/tools/package.py" VERBATIM)
target_sources(__idf_main PRIVATE "${exports}")
add_custom_target(app_export_table DEPENDS "${exports}")
add_dependencies(app_export_table ${image_targets})
add_dependencies(__idf_main app_export_table)
