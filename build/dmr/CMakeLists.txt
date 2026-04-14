cmake_minimum_required(VERSION 3.14)

project(DMR)

set(CMAKE_C_COMPILER mpicc)

set(LIB_NAME dmr)
set(LIB_VERSION 2.0.0)

# Helper macro to read from environment or -DMY_VAR
macro(set_from_env_or_default var env_var)
    if(NOT DEFINED ${var} AND DEFINED ENV{${env_var}})
        set(${var} "$ENV{${env_var}}")
    endif()
endmacro()

# Consistent check to determine if a specific variable is true or false
macro(is_true var result_var)
    if(DEFINED ${var})
        string(TOLOWER "${${var}}" _truthy_val)
        set(_truthy_values "1" "true" "yes" "on")
        list(FIND _truthy_values "${_truthy_val}" _truthy_index)
        if(_truthy_index GREATER -1)
            set(${result_var} TRUE)
        else()
            set(${result_var} FALSE)
        endif()
    else()
        set(${result_var} FALSE)
    endif()
endmacro()

# Whether or not we are compiling against SLURM4DMR
set_from_env_or_default(SLURM4DMR SLURM4DMR)
is_true(SLURM4DMR SLURM4DMR_ENABLED)
if(SLURM4DMR_ENABLED)
    add_compile_definitions(SLURM4DMR=1)
    message(STATUS "Compiling with Slurm4DMR")
endif()

# Whether or not we are compiling against TALP
set_from_env_or_default(DMR_USE_TALP DMR_USE_TALP)
is_true(DMR_USE_TALP TALP_ENABLED)
if(TALP_ENABLED)
    add_compile_definitions(COMPILED_WITH_TALP=1)
    message(STATUS "Compiling with TALP")
endif()

# Prints debug statements if 0 or 1. See dmr.h
set_from_env_or_default(DMR_DEBUG_LEVEL DMR_DEBUG_LEVEL)
if(DEFINED DMR_DEBUG_LEVEL)
    add_compile_definitions(DMR_DEBUG_LEVEL=${DMR_DEBUG_LEVEL})
    message(STATUS "Set DMR_DEBUG_LEVEL to ${DMR_DEBUG_LEVEL}")
endif()

# Whether or not to print analytics information
set_from_env_or_default(DMR_PRINT_ANALYTICS DMR_PRINT_ANALYTICS)
is_true(DMR_PRINT_ANALYTICS DMR_PRINT_ANALYTICS_ENABLED)
if(DEFINED DMR_PRINT_ANALYTICS)
    if(DMR_PRINT_ANALYTICS_ENABLED)
        add_compile_definitions(DMR_PRINT_ANALYTICS=1)
        message(STATUS "Set DMR_PRINT_ANALYTICS to 1")
    else()
        add_compile_definitions(DMR_PRINT_ANALYTICS=0)
        message(STATUS "Set DMR_PRINT_ANALYTICS to 0")
    endif()
endif()

# Number of nodes to add in each expander job by default
set_from_env_or_default(DMR_NODES_IN_EXPAND DMR_NODES_IN_EXPAND)
if(DEFINED DMR_NODES_IN_EXPAND)
    add_compile_definitions(DMR_NODES_IN_EXPAND=${DMR_NODES_IN_EXPAND})
    message(STATUS "Set DMR_NODES_IN_EXPAND to ${DMR_NODES_IN_EXPAND}")
endif()

# Number of processes to add per node in each expander job by default
set_from_env_or_default(DMR_PROCS_PER_NODE DMR_PROCS_PER_NODE)
if(DEFINED DMR_PROCS_PER_NODE)
    add_compile_definitions(DMR_PROCS_PER_NODE=${DMR_PROCS_PER_NODE})
    message(STATUS "Set DMR_PROCS_PER_NODE to ${DMR_PROCS_PER_NODE}")
endif()

# Number of nodes to remove in each shrink by default
set_from_env_or_default(DMR_NODES_IN_SHRINK DMR_NODES_IN_SHRINK)
if(DEFINED DMR_NODES_IN_SHRINK)
    add_compile_definitions(DMR_NODES_IN_SHRINK=${DMR_NODES_IN_SHRINK})
    message(STATUS "Set DMR_NODES_IN_SHRINK to ${DMR_NODES_IN_SHRINK}")
endif()

# Whether or not to use checkpoint restart for shrinking
set_from_env_or_default(CHECKPOINT_RESTART CHECKPOINT_RESTART)
is_true(CHECKPOINT_RESTART CHECKPOINT_RESTART_ENABLED)
if(DEFINED CHECKPOINT_RESTART)
    # Use the boolean result for compile definition (0 or 1)
    if(CHECKPOINT_RESTART_ENABLED)
        add_compile_definitions(CHECKPOINT_RESTART=1)
        message(STATUS "Set CHECKPOINT_RESTART to 1")
    else()
        add_compile_definitions(CHECKPOINT_RESTART=0)
        message(STATUS "Set CHECKPOINT_RESTART to 0")
    endif()
elseif(SLURM4DMR_ENABLED)
    set(CHECKPOINT_RESTART 0)
    add_compile_definitions(CHECKPOINT_RESTART=0)
    message(STATUS "Automatically set CHECKPOINT_RESTART to 0 (reason: Slurm4DMR enabled)")
endif()

# Cooldown to wait until spawning onto jobs that just started running
set_from_env_or_default(WAIT_FOR_SLURM_S WAIT_FOR_SLURM_S)
if(DEFINED WAIT_FOR_SLURM_S)
    add_compile_definitions(WAIT_FOR_SLURM_S=${WAIT_FOR_SLURM_S})
    message(STATUS "Set WAIT_FOR_SLURM_S to ${WAIT_FOR_SLURM_S}")
endif()

# Whether or not we can shrink Slurm jobs
set_from_env_or_default(JOBS_CAN_SHRINK JOBS_CAN_SHRINK)
is_true(JOBS_CAN_SHRINK JOBS_CAN_SHRINK_ENABLED)
if(DEFINED JOBS_CAN_SHRINK)
    if(JOBS_CAN_SHRINK_ENABLED)
        add_compile_definitions(JOBS_CAN_SHRINK=1)
        message(STATUS "Set JOBS_CAN_SHRINK to 1")
    else()
        add_compile_definitions(JOBS_CAN_SHRINK=0)
        message(STATUS "Set JOBS_CAN_SHRINK to 0")
    endif()
else()
    set(JOBS_CAN_SHRINK_ENABLED 1)
    add_compile_definitions(JOBS_CAN_SHRINK=1)
    message(STATUS "Automatically set JOBS_CAN_SHRINK to 1 (reason: default assumption)")
endif()

# Whether or not we can grow Slurm jobs (requires legacy Slurm)
set_from_env_or_default(JOBS_CAN_GROW JOBS_CAN_GROW)
is_true(JOBS_CAN_GROW JOBS_CAN_GROW_ENABLED)
if(DEFINED JOBS_CAN_GROW)
    if(JOBS_CAN_GROW_ENABLED)
        if(NOT JOBS_CAN_SHRINK_ENABLED)
            message(FATAL_ERROR "JOBS_CAN_SHRINK feature is required for JOBS_CAN_GROW feature")
        endif()
        add_compile_definitions(JOBS_CAN_GROW=1)
        message(STATUS "Set JOBS_CAN_GROW to 1")
    else()
        add_compile_definitions(JOBS_CAN_GROW=0)
        message(STATUS "Set JOBS_CAN_GROW to 0")
    endif()
elseif(SLURM4DMR_ENABLED AND JOBS_CAN_SHRINK_ENABLED)
    add_compile_definitions(JOBS_CAN_GROW=1)
    set(JOBS_CAN_GROW 1)
    message(STATUS "Automatically set JOBS_CAN_GROW to 1 (reason: Slurm4DMR and job shrinking enabled)")
endif()

# Whether or not a resource request, i.e. dmr_check(SHOULD_EXPAND), blocks until resources acquired
set_from_env_or_default(DMR_BLOCKING_REQ DMR_BLOCKING_REQ)
is_true(DMR_BLOCKING_REQ DMR_BLOCKING_REQ_ENABLED)
if(DEFINED DMR_BLOCKING_REQ)
    if(DMR_BLOCKING_REQ_ENABLED)
        add_compile_definitions(DMR_BLOCKING_REQ=1)
        message(STATUS "Set DMR_BLOCKING_REQ to 1")
    else()
        add_compile_definitions(DMR_BLOCKING_REQ=0)
        message(STATUS "Set DMR_BLOCKING_REQ to 0")
    endif()
elseif(SLURM4DMR_ENABLED)
    set(DMR_BLOCKING_REQ_ENABLED TRUE)
    add_compile_definitions(DMR_BLOCKING_REQ=1)
    message(STATUS "Automatically set DMR_BLOCKING_REQ to 1 (reason: Slurm4DMR enabled)")
endif()

# Custom path where we can find binary files from Slurm
set_from_env_or_default(CUSTOM_SLURM_BIN_PREFIX CUSTOM_SLURM_BIN_PREFIX)
if(DEFINED CUSTOM_SLURM_BIN_PREFIX)
    add_compile_definitions(CUSTOM_SLURM_BIN_PREFIX=\"${CUSTOM_SLURM_BIN_PREFIX}\")
    message(STATUS "Set CUSTOM_SLURM_BIN_PREFIX to ${CUSTOM_SLURM_BIN_PREFIX}")
endif()

# TALP policy sensitivity factor, included for completeness
set_from_env_or_default(DMR_TALP_SENSITIVITY DMR_TALP_SENSITIVITY)
if(DEFINED DMR_TALP_SENSITIVITY)
    add_compile_definitions(DMR_TALP_SENSITIVITY=${DMR_TALP_SENSITIVITY})
    message(STATUS "Set DMR_TALP_SENSITIVITY to ${DMR_TALP_SENSITIVITY}")
endif()

# TALP policy target CE, included for completeness
set_from_env_or_default(DMR_TALP_TARGET_CE DMR_TALP_TARGET_CE)
if(DEFINED DMR_TALP_TARGET_CE)
    add_compile_definitions(DMR_TALP_TARGET_CE=${DMR_TALP_TARGET_CE})
    message(STATUS "Set DMR_TALP_TARGET_CE to ${DMR_TALP_TARGET_CE}")
endif()

# Where to find the Slurm .so files (if not running with Slurm4DMR)
set_from_env_or_default(SLURM_LIB SLURM_LIB)

# Where to find the Slurm include files (if not running with Slurm4DMR)
set_from_env_or_default(SLURM_INCLUDE SLURM_INCLUDE)

# Root directory of Slurm4DMR installation
set_from_env_or_default(SLURM_ROOT SLURM_ROOT)

# Where to find DLB files if manually specified
set_from_env_or_default(DLB_ROOT DLB_ROOT)

# We want to compile against Slurm4DMR library
if(SLURM4DMR_ENABLED)

    if(NOT DEFINED SLURM_ROOT)
        message(FATAL_ERROR "SLURM4DMR enabled but SLURM_ROOT is not set.")
    endif()

    set(SLURM_LIB_DIR "${SLURM_ROOT}/lib")

    # Define where to find custom squeue command, ensuring the string is passed correctly with a workaround approach
    if(NOT DEFINED CUSTOM_SLURM_BIN_PREFIX)
        set(CUSTOM_SLURM_BIN_PREFIX "${SLURM_ROOT}/bin")
        add_compile_definitions(CUSTOM_SLURM_BIN_PREFIX=\"${CUSTOM_SLURM_BIN_PREFIX}\")
        message(STATUS "Automatically set CUSTOM_SLURM_BIN_PREFIX to ${CUSTOM_SLURM_BIN_PREFIX} (reason: Slurm4DMR enabled)")
    endif()

# We have manually set location of Slurm .so files
elseif(DEFINED SLURM_LIB_DIR)
    set(SLURM_LIB_DIR SLURM_LIB)
    message(STATUS "Read SLURM_LIB from environment: " ${SLURM_LIB})

# Try to autodetect based on what the sbatch command is linking to
else()
    execute_process(
        COMMAND bash -c "ldd $(which sbatch) | grep libslurm | awk '{print $3}'"
        OUTPUT_VARIABLE SLURM_SO_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(SLURM_SO_PATH)
        get_filename_component(SLURM_LIB_DIR "${SLURM_SO_PATH}" DIRECTORY)
        message(STATUS "Auto-detected Slurm libraries")
    else()
        message(FATAL_ERROR "Failed to automatically detect Slurm libraries. Try to set SLURM_LIB manually.")
    endif()
endif()

# Check for existance of libslurmfull or just libslurm; error out if neither exists
if(EXISTS "${SLURM_LIB_DIR}")
    if(EXISTS "${SLURM_LIB_DIR}/libslurmfull.so")
        set(SLURM_LIB_NAME "slurmfull")
    elseif(EXISTS "${SLURM_LIB_DIR}/libslurm.so")
        set(SLURM_LIB_NAME "slurm")
    else()
        message(FATAL_ERROR "No libslurmfull.so or libslurm.so found in ${SLURM_LIB_DIR}")
    endif()
else()
    message(FATAL_ERROR "SLURM_LIB_DIR does not exist: ${SLURM_LIB_DIR}")
endif()

# Find Slurm include files, which may be in a different location than library files
if(DEFINED SLURM_INCLUDE)
    set(SLURM_INCLUDE_DIR ${SLURM_INCLUDE})
    message(STATUS "Using user-specified SLURM_INCLUDE: ${SLURM_INCLUDE_DIR}")
elseif(SLURM4DMR_ENABLED)
    set(SLURM_INCLUDE_DIR "${SLURM_ROOT}/include")
    message(STATUS "Using include files in SLURM_ROOT directory: ${SLURM_INCLUDE_DIR}")
else()
    find_path(SLURM_INCLUDE_DIR
        NAMES slurm/slurm.h
        PATH_SUFFIXES include
    )
    if(SLURM_INCLUDE_DIR)
        message(STATUS "Auto-detected Slurm include dir: ${SLURM_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR "Could not find slurm/slurm.h. Set SLURM_INCLUDE environment variable.")
    endif()
endif()

# Check that we found something reasonable for Slurm headers
if(NOT EXISTS "${SLURM_INCLUDE_DIR}/slurm/slurm.h")
    message(FATAL_ERROR "Invalid SLURM_INCLUDE_DIR: ${SLURM_INCLUDE_DIR} does not contain slurm/slurm.h")
endif()

# If using a custom path to squeue, check that scontrol exists inside it
if(DEFINED CUSTOM_SLURM_BIN_PREFIX AND NOT EXISTS "${CUSTOM_SLURM_BIN_PREFIX}/scontrol")
    message(FATAL_ERROR "Could not find scontrol executable in ${CUSTOM_SLURM_BIN_PREFIX}")
endif()

# Get DLB paths if we are compiling against it
if(TALP_ENABLED)
    set(DLB_FOUND FALSE)
    set(DLB_INCLUDE_DIR "")
    set(DLB_LIBRARY_DIR "")

    # Best option: try to find from DLB_ROOT
    if(DEFINED DLB_ROOT)
        message(STATUS "DLB_ROOT found: ${DLB_ROOT}")
        set(DLB_INCLUDE_DIR "${DLB_ROOT}/include")
        set(DLB_LIBRARY_DIR "${DLB_ROOT}/lib")
        set(DLB_FOUND TRUE)

    # No DLB_ROOT set. Try to autodetect using 'which dlb'
    else()
        execute_process(
            COMMAND which dlb
            OUTPUT_VARIABLE DLB_BIN_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )

        if(DLB_BIN_PATH)
            get_filename_component(DLB_BIN_DIR ${DLB_BIN_PATH} DIRECTORY)
            get_filename_component(DLB_ROOT ${DLB_BIN_DIR} DIRECTORY)
            set(DLB_INCLUDE_DIR "${DLB_ROOT}/include")
            set(DLB_LIBRARY_DIR "${DLB_ROOT}/lib")
            message(STATUS "Autodetected DLB at: ${DLB_ROOT}")
            set(DLB_FOUND TRUE)
        endif()
    endif()

    if(NOT DLB_FOUND)
        message(FATAL_ERROR "Could not find DLB for TALP. Try to set DLB_ROOT to its root directory.")
    endif()

endif()

include_directories(include)

add_compile_options(-g -O3 -fPIC -Wall -Wextra -pedantic)

# --- Source files -- #
file(GLOB SRCS "src/*.c")

# NEW: include DQR sources
file(GLOB DQR_SRCS "src/dqr/*.c")
list(APPEND SRCS ${DQR_SRCS})

set(BUILD_DIR "${CMAKE_BINARY_DIR}/build")
set(LIB_DIR "${CMAKE_BINARY_DIR}/lib")
# --- Source files -- #

add_library(${LIB_NAME} SHARED ${SRCS})

set(CMAKE_BUILD_RPATH "${SLURM_LIB_DIR}")
set(INSTALL_RPATH "${SLURM_LIB_DIR}")

if(TALP_ENABLED)
    list(APPEND CMAKE_BUILD_RPATH "${DLB_LIBRARY_DIR}")
    list(APPEND INSTALL_RPATH "${DLB_LIBRARY_DIR}")
endif()

set_target_properties(${LIB_NAME} PROPERTIES
    VERSION ${LIB_VERSION}
    SOVERSION "${LIB_VERSION}"
    OUTPUT_NAME "${LIB_NAME}"
    LIBRARY_OUTPUT_DIRECTORY ${LIB_DIR}
    INSTALL_RPATH "${INSTALL_RPATH}"
    BUILD_RPATH "${CMAKE_BUILD_RPATH}"
    BUILD_WITH_INSTALL_RPATH FALSE
)

option(DMR_ENABLE_QCORE "Enable QCore backend tool integration" ON)

if(DMR_ENABLE_QCORE)
    get_filename_component(_DMR_TOP "${CMAKE_BINARY_DIR}" DIRECTORY)
    set(_QCORE_PREFIX "${_DMR_TOP}/tools/QCore/install")

    set(_QCORE_DIR_LIB   "${_QCORE_PREFIX}/lib/cmake/qcore")
    set(_QCORE_DIR_LIB64 "${_QCORE_PREFIX}/lib64/cmake/qcore")

    if(EXISTS "${_QCORE_DIR_LIB}/qcoreConfig.cmake")
        set(_QCORE_DIR "${_QCORE_DIR_LIB}")
        set(_QCORE_LIBDIR "${_QCORE_PREFIX}/lib")
    elseif(EXISTS "${_QCORE_DIR_LIB64}/qcoreConfig.cmake")
        set(_QCORE_DIR "${_QCORE_DIR_LIB64}")
        set(_QCORE_LIBDIR "${_QCORE_PREFIX}/lib64")
    else()
        message(FATAL_ERROR
            "QCore not found.\n"
            "Checked:\n"
            "  ${_QCORE_DIR_LIB}\n"
            "  ${_QCORE_DIR_LIB64}\n"
            "Build/install QCore first."
        )
    endif()

    list(PREPEND CMAKE_PREFIX_PATH "${_QCORE_PREFIX}")
    set(qcore_DIR "${_QCORE_DIR}" CACHE PATH "Path to qcoreConfig.cmake" FORCE)

    message(STATUS "QCore: using vendored install at ${_QCORE_PREFIX}")
    message(STATUS "QCore: config dir = ${_QCORE_DIR}")
    message(STATUS "QCore: lib dir    = ${_QCORE_LIBDIR}")

    find_package(qcore CONFIG REQUIRED)
    target_link_libraries(${LIB_NAME} PRIVATE qcore::qcore qcore::qtensor)
    target_compile_definitions(${LIB_NAME} PRIVATE DMR_HAVE_QCORE=1)

    set_property(TARGET ${LIB_NAME} APPEND PROPERTY BUILD_RPATH   "${_QCORE_LIBDIR}")
    set_property(TARGET ${LIB_NAME} APPEND PROPERTY INSTALL_RPATH "${_QCORE_LIBDIR}")
endif()


# Slurm include directories
target_include_directories(${LIB_NAME} PRIVATE ${SLURM_INCLUDE_DIR})

# Link with math and Slurm
target_link_libraries(${LIB_NAME}
    PRIVATE
    ${SLURM_LIB_DIR}/lib${SLURM_LIB_NAME}.so
    m
) 

# Link DLB if applicable
if(TALP_ENABLED)
    target_include_directories(${LIB_NAME} PRIVATE ${DLB_INCLUDE_DIR})
    # Link this to avoid compilation issues, preload libdlb_mpi.so to avoid runtime issues
    # This seems to hold true at least on MareNostrum5, not sure why
    target_link_libraries(${LIB_NAME} PRIVATE ${DLB_LIBRARY_DIR}/libdlb.so)
endif()

# Default to installing into main DMR directory
set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}" CACHE PATH "Install path" FORCE)

install(TARGETS ${LIB_NAME}
    LIBRARY DESTINATION lib
)

# Cleanup
add_custom_target(clean-all
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${LIB_DIR} ${BUILD_DIR}
    COMMENT "Cleaning build and lib directories"
)
