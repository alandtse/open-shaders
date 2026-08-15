# Shared definition of the Alpha/Beta release-stage rule. Included by the build
# (CMakeLists.txt, feeding FEATURE_ALPHA_NAMES/FEATURE_BETA_NAMES) and by the
# standalone emitter (cmake/emit-default-disabled-features.cmake) so there is
# ONE implementation; the CI guard then verifies the Python cache builder
# (tools/build-shader-cache.py's default_disabled_features(), via
# tools/feature_version_audit.py's line-anchored match) against this same logic.

# Sets ${out_alpha_var}/${out_beta_var} to TRUE/FALSE from a feature .ini's raw
# text. Alpha takes precedence when both are set; absent or non-truthy means
# Release. Anchored to a line start ((^|[\r\n]); CMake regex has no multiline
# mode) so a settings key ending in "alpha"/"beta" (e.g. WaterAlpha = 1) is not
# mistaken for a stage flag. Keep the regex in sync with
# tools/feature_version_audit.py's line-anchored match.
function(feature_ini_stage content out_alpha_var out_beta_var)
    set(_alpha "")
    set(_beta "")
    # NOTE: guard on the match-result variable, not CMAKE_MATCH_2 directly --
    # CMAKE_MATCH_2 retains its prior value from an unrelated regex when this
    # REGEX MATCH misses.
    string(
        REGEX MATCH "(^|[\r\n])[ \t]*[Aa]lpha[ \t]*=[ \t]*([A-Za-z0-9]+)"
        _alpha_match
        "${content}"
    )
    if(_alpha_match)
        string(TOLOWER "${CMAKE_MATCH_2}" _alpha)
    endif()
    string(
        REGEX MATCH "(^|[\r\n])[ \t]*[Bb]eta[ \t]*=[ \t]*([A-Za-z0-9]+)"
        _beta_match
        "${content}"
    )
    if(_beta_match)
        string(TOLOWER "${CMAKE_MATCH_2}" _beta)
    endif()
    if(_alpha MATCHES "^(true|1|yes|on)$")
        set(${out_alpha_var} TRUE PARENT_SCOPE)
    else()
        set(${out_alpha_var} FALSE PARENT_SCOPE)
    endif()
    if(NOT _alpha MATCHES "^(true|1|yes|on)$" AND _beta MATCHES "^(true|1|yes|on)$")
        set(${out_beta_var} TRUE PARENT_SCOPE)
    else()
        set(${out_beta_var} FALSE PARENT_SCOPE)
    endif()
endfunction()
