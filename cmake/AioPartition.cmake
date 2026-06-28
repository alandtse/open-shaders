# Shared definition of the AIO feature-partition rule. Included by the build
# (CMakeLists.txt) and by the standalone emitter (cmake/emit-aio-features.cmake)
# so there is ONE implementation; the CI guard then verifies the Python cache
# builder (tools/build-shader-cache.py) against this same logic.

# TRUE in ${out_var} if the feature ships in the AIO: it is CORE, or its `.ini`
# sets `autoupload`/`aio` to a truthy value (true/1/yes/on). `aio` bundles a
# feature without a standalone upload (the matrix upload is gated on autoupload).
function(feature_in_aio feature_path out_var)
    if(EXISTS "${feature_path}/CORE")
        set(${out_var} TRUE PARENT_SCOPE)
        return()
    endif()
    file(GLOB _ini_candidates "${feature_path}/Shaders/Features/*.ini")
    foreach(_ini IN LISTS _ini_candidates)
        file(STRINGS "${_ini}" _ini_lines REGEX "^[ \t]*(autoupload|aio)[ \t]*=")
        foreach(_line IN LISTS _ini_lines)
            # Strip "key =" to extract the value, lowercase it.
            string(
                REGEX REPLACE "^[ \t]*(autoupload|aio)[ \t]*=[ \t]*" "" _val "${_line}"
            )
            string(STRIP "${_val}" _val)
            string(TOLOWER "${_val}" _val)
            # Treat unset / falsey values as off; only explicit truthy values count.
            if(_val STREQUAL "true"
               OR _val STREQUAL "1"
               OR _val STREQUAL "yes"
               OR _val STREQUAL "on"
            )
                set(${out_var} TRUE PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endforeach()
    set(${out_var} FALSE PARENT_SCOPE)
endfunction()
