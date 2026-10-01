# Generate multi-resolution app artwork, including Retina.
get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
set(iconset "${output_directory}/AppIcon.iconset")
file(MAKE_DIRECTORY "${iconset}")
foreach(size 16 32 128 256 512)
    math(EXPR retina_size "${size} * 2")
    execute_process(COMMAND "${SIPS}" -z "${size}" "${size}" "${INPUT}"
        --out "${iconset}/icon_${size}x${size}.png"
        OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${SIPS}" -z "${retina_size}" "${retina_size}" "${INPUT}"
        --out "${iconset}/icon_${size}x${size}@2x.png"
        OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
endforeach()
execute_process(COMMAND "${ICONUTIL}" -c icns -o "${OUTPUT}" "${iconset}"
    COMMAND_ERROR_IS_FATAL ANY)
file(REMOVE_RECURSE "${iconset}")
