# The game and/or Visual Studio debugger may hold the destination PDB open.
# A locked debug-symbol file must not cause a successfully built and deployed
# CommunityShaders.dll to be reported as a failed build. DLL copying remains
# strict in the caller; only this optional PDB copy may be skipped.
if(NOT DEFINED SRC OR NOT DEFINED DST OR SRC STREQUAL "" OR DST STREQUAL "")
    message(FATAL_ERROR "CopyOptionalPDB: SRC and DST paths are required")
endif()

if(NOT EXISTS "${SRC}")
    message(
        WARNING
        "Optional PDB source is missing: ${SRC}. DLL deployment continues."
    )
    return()
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SRC}" "${DST}"
    RESULT_VARIABLE _pdb_copy_result
    OUTPUT_VARIABLE _pdb_copy_output
    ERROR_VARIABLE _pdb_copy_error
)

if(NOT "${_pdb_copy_result}" STREQUAL "0")
    string(STRIP "${_pdb_copy_error}" _pdb_copy_error)
    message(
        WARNING
        "Optional PDB copy skipped (destination may be locked by Skyrim or Visual Studio). "
        "The DLL was deployed; the installed PDB may be older than the DLL. "
        "Close the process/debugger to refresh symbols on a subsequent deploy. "
        "Destination: ${DST}. Copy result: ${_pdb_copy_result}. ${_pdb_copy_error}"
    )
endif()
