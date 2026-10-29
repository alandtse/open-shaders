include(FetchContent)
FetchContent_Declare(
    OpenWind
    GIT_REPOSITORY https://github.com/kevdevrev/open-wind.git
    GIT_TAG 4ea41b97cf0b4f77ffbd05567a466421ec5f7200
    GIT_SUBMODULES "" SOURCE_SUBDIR api
)
FetchContent_MakeAvailable(OpenWind)
