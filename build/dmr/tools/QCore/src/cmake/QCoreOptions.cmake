option(QCORE_USE_FETCHCONTENT "Fetch dependencies via FetchContent" ON)

# Backends
option(QCORE_ENABLE_QULACS "Enable CPU backend via Qulacs" ON)
option(QCORE_FETCH_QULACS "Fetch & build Qulacs from source" ON)

option(QCORE_ENABLE_CUQUANTUM "Enable cuQuantum backend" OFF)

# Fail configure if requested deps are missing
option(QCORE_STRICT_DEPS "Fail configure if requested deps are missing" ON)
