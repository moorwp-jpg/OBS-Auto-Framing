function(set_target_properties_plugin target)
    set(output_name "${target}")
    set(args ${ARGN})
    list(LENGTH args args_length)

    while(args_length GREATER 0)
        list(POP_FRONT args key)
        if(key STREQUAL "PROPERTIES")
            # Match the official template call shape.
        elseif(key STREQUAL "OUTPUT_NAME")
            list(POP_FRONT args output_name)
        endif()
        list(LENGTH args args_length)
    endwhile()

    set_target_properties(
        ${target}
        PROPERTIES PREFIX "" OUTPUT_NAME "${output_name}" FOLDER "plugins")

    if(WIN32)
        set(plugin_binary_destination "obs-plugins/64bit")
        set(plugin_data_destination "data/obs-plugins/${output_name}")
    else()
        set(plugin_binary_destination "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
        set(plugin_data_destination "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${output_name}")
    endif()

    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND
            "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "$<TARGET_FILE_DIR:${target}>/data/obs-plugins/${output_name}"
        COMMENT "Copying ${output_name} plugin data")

    install(TARGETS ${target} RUNTIME DESTINATION "${plugin_binary_destination}" LIBRARY DESTINATION "${plugin_binary_destination}")
    install(
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
        DESTINATION "${plugin_data_destination}"
        OPTIONAL)
endfunction()
