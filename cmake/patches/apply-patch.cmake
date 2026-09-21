# Idempotent patch helper for FetchContent PATCH_COMMANDs:
#
#   cmake -DPATCH_FILE=<file.patch> -P apply-patch.cmake      (run in the source directory)
#
# Applies the unified diff with `patch -p1 --forward`, or does nothing when every file of
# it is already patched. Fails the configure loudly when the patch neither applies nor is
# already applied, so an unpatched dependency can never be built silently.
#
# `patch --forward --dry-run` is used for the probe on purpose: BSD/Apple patch answers the
# "Reversed (or previously applied) patch detected! Ignore -R?" question with "yes" when it is
# not attached to a terminal, which makes `patch -R --dry-run` succeed on an UNpatched tree.
if(NOT PATCH_FILE OR NOT EXISTS "${PATCH_FILE}")
    message(FATAL_ERROR "apply-patch.cmake: PATCH_FILE '${PATCH_FILE}' not found")
endif()
find_program(GX_PATCH_EXECUTABLE patch REQUIRED)

execute_process(
    COMMAND "${GX_PATCH_EXECUTABLE}" -p1 --forward --dry-run -i "${PATCH_FILE}"
    RESULT_VARIABLE _probe_rc OUTPUT_VARIABLE _probe_out ERROR_VARIABLE _probe_err)

if(_probe_rc EQUAL 0)
    execute_process(
        COMMAND "${GX_PATCH_EXECUTABLE}" -p1 --forward -i "${PATCH_FILE}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "apply-patch: failed to apply ${PATCH_FILE}:\n${_out}\n${_err}")
    endif()
    message(STATUS "apply-patch: applied ${PATCH_FILE}")
    return()
endif()

# Not applicable as is. That is fine only if EVERY file of the patch is already patched:
# GNU patch says "Reversed (or previously applied) patch detected!  Skipping patch." and
# Apple patch says "Ignoring previously applied (or reversed) patch." once per file.
set(_all "${_probe_out}\n${_probe_err}")
string(REGEX MATCHALL "patching file" _files "${_all}")
string(REGEX MATCHALL "[Pp]reviously applied" _skipped "${_all}")
list(LENGTH _files _n_files)
list(LENGTH _skipped _n_skipped)
if(_n_files GREATER 0 AND _n_files EQUAL _n_skipped AND NOT _all MATCHES "FAILED|can't find file")
    message(STATUS "apply-patch: ${PATCH_FILE} already applied")
    return()
endif()
message(FATAL_ERROR "apply-patch: ${PATCH_FILE} neither applies nor is already applied:\n${_all}")
