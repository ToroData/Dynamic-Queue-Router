cmake_minimum_required(VERSION 3.14)
project(qcore VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---- Options ----
option(QCORE_USE_FETCHCONTENT "Fetch dependencies via FetchContent" ON)

# Backends
option(QCORE_ENABLE_QULACS "Enable Qulacs CPU backend support" ON)
option(QCORE_FETCH_QULACS "Fetch & build Qulacs from source" ON)

option(QCORE_ENABLE_CUQUANTUM "Enable CUDA/cuQuantum support" OFF)

# Parsers
option(QCORE_ENABLE_STAQ     "Enable OpenQASM2 parsing via staq" ON)
option(QCORE_ENABLE_QASM3TOOLS "Enable OpenQASM3 parsing via qasm3tools" ON)

# Fail configure if requested deps are missing
option(QCORE_STRICT_DEPS "Fail configure if requested deps are missing" ON)

# Allow parent project to override options in subprojects (FetchContent)
if(POLICY CMP0077)
  cmake_policy(SET CMP0077 NEW)
endif()


# ---- Dependencies: nlohmann/json ----
if(QCORE_USE_FETCHCONTENT)
  include(FetchContent)
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
  )
  FetchContent_MakeAvailable(nlohmann_json)
  set(QCORE_NLOHMANN_INCLUDE_DIR "${nlohmann_json_SOURCE_DIR}/include")
  install(
    DIRECTORY "${nlohmann_json_SOURCE_DIR}/include/nlohmann"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  )
else()
  find_package(nlohmann_json REQUIRED)
endif()



# ---- Core library sources (ONLY existing ones) ----
add_library(qcore SHARED
  src/qcore_api.cpp
  src/qcore_validate.cpp
  src/qcore_result.cpp
  src/qasm_mini_parser.cpp
  src/backends/backend_dispatch.cpp
  src/backends/backend_cpu_qulacs.cpp
  src/backends/backend_gpu_cuquantum.cpp
  src/backends/backend_qpu_qulacs.cpp
)
add_library(qcore::qcore ALIAS qcore)

target_include_directories(qcore
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_include_directories(qcore PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ---- qtensor library ----
add_library(qtensor STATIC
  src/qtensor/qtensor.cpp
  src/qtensor/qtensor_api.cpp
)
add_library(qcore::qtensor ALIAS qtensor)

target_include_directories(qtensor
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(QCORE_USE_FETCHCONTENT AND QCORE_NLOHMANN_INCLUDE_DIR)
  target_include_directories(qcore  PRIVATE "${QCORE_NLOHMANN_INCLUDE_DIR}")
  target_include_directories(qtensor PRIVATE "${QCORE_NLOHMANN_INCLUDE_DIR}")
endif()

target_compile_features(qtensor PUBLIC cxx_std_17)
set_target_properties(qtensor PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(qcore PUBLIC
  QCORE_ENABLE_QULACS=$<BOOL:${QCORE_ENABLE_QULACS}>
  QCORE_ENABLE_CUQUANTUM=$<BOOL:${QCORE_ENABLE_CUQUANTUM}>
)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Qulacs (CPU) ----
if(QCORE_ENABLE_QULACS)
  if(NOT QCORE_FETCH_QULACS)
    message(FATAL_ERROR "This project expects QCORE_FETCH_QULACS=ON (no FindQulacs provided).")
  endif()

  include(FetchContent)

  set(USE_PYTHON OFF CACHE BOOL "" FORCE)
  set(USE_TEST   OFF CACHE BOOL "" FORCE)
  set(USE_MPI    OFF CACHE BOOL "" FORCE)
  set(USE_GPU    OFF CACHE BOOL "" FORCE)
  set(USE_OMP    ON  CACHE BOOL "" FORCE)

  FetchContent_Declare(qulacs
    GIT_REPOSITORY https://github.com/qulacs/qulacs.git
    GIT_TAG v0.6.6
  )
  FetchContent_MakeAvailable(qulacs)
  target_include_directories(qcore PRIVATE
    ${qulacs_SOURCE_DIR}/include
  )

  if(NOT TARGET cppsim_static)
    message(FATAL_ERROR "Expected Qulacs target 'cppsim_static' not found. Check Qulacs version/config.")
  endif()

  target_link_libraries(qcore PRIVATE cppsim_static)
  target_compile_definitions(qcore PUBLIC QCORE_HAS_QULACS=1)

  find_package(OpenMP REQUIRED)
  target_link_libraries(qcore PUBLIC OpenMP::OpenMP_CXX)
endif()



# ---- cuQuantum (GPU) ----
if(QCORE_ENABLE_CUQUANTUM)
  find_package(CUDAToolkit REQUIRED)


  find_path(CUQUANTUM_INCLUDE_DIR
    NAMES custatevec.h cutensornet.h cudensitymat.h
    PATHS
      /usr/include
      /usr/include/cuquantum
      /usr/local/include
  )
  if(NOT CUQUANTUM_INCLUDE_DIR)
    message(FATAL_ERROR "cuQuantum headers not found (custatevec.h/cutensornet.h). Install libcuquantum0-dev-cuda-12.")
  endif()

  find_library(CUQUANTUM_CUSTATEVEC_LIB
    NAMES custatevec
    PATHS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu
    REQUIRED
  )
  find_library(CUQUANTUM_CUTENSORNET_LIB
    NAMES cutensornet
    PATHS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu
  )
  find_library(CUQUANTUM_CUDENSITYMAT_LIB
    NAMES cudensitymat
    PATHS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu
  )

  target_include_directories(qcore PUBLIC "${CUQUANTUM_INCLUDE_DIR}")
  target_link_libraries(qcore PRIVATE CUDA::cudart ${CUQUANTUM_CUSTATEVEC_LIB})
  target_compile_definitions(qcore PUBLIC QCORE_HAS_CUQUANTUM=1)

  if(CUQUANTUM_CUTENSORNET_LIB)
    target_link_libraries(qcore PRIVATE ${CUQUANTUM_CUTENSORNET_LIB})
    target_compile_definitions(qcore PUBLIC QCORE_HAS_CUTENSORNET=1)
  endif()

  if(CUQUANTUM_CUDENSITYMAT_LIB)
    target_link_libraries(qcore PRIVATE ${CUQUANTUM_CUDENSITYMAT_LIB})
    target_compile_definitions(qcore PUBLIC QCORE_HAS_CUDENSITYMAT=1)
  endif()
endif()


# ---- CLI tool ----
add_executable(qcore_cli tools/qcore_cli.cpp)
target_link_libraries(qcore_cli PRIVATE qcore)
set_target_properties(qcore_cli PROPERTIES
  BUILD_RPATH   "$ORIGIN/../lib"
  INSTALL_RPATH "$ORIGIN/../lib"
)

# ---- Install Python backend script for QPU ----
install(FILES
  src/backends/backend_qpu_qulacs.py
  src/backends/backend_qpu_ibm.py
  DESTINATION ${CMAKE_INSTALL_LIBDIR}
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
              GROUP_READ GROUP_EXECUTE
              WORLD_READ WORLD_EXECUTE
)

# ---- Install targets ----
install(TARGETS qcore qtensor
  EXPORT qcoreTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(TARGETS qcore_cli
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY src/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# ---- CMake package config for consumers ----
install(EXPORT qcoreTargets
  FILE qcoreTargets.cmake
  NAMESPACE qcore::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qcore
)

# Config and Version files
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qcoreConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/qcoreConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qcore
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/qcoreConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/qcoreConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/qcoreConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qcore
)
