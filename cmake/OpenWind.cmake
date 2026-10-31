include(FetchContent)
FetchContent_Declare(
    OpenWind
    GIT_REPOSITORY https://github.com/kevdevrev/open-wind.git
    GIT_TAG d1896c87277b33f6a1661adef353ac31dab6caa9
    GIT_SUBMODULES "" SOURCE_SUBDIR api
)
FetchContent_MakeAvailable(OpenWind)
