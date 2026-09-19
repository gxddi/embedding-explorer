cmake_minimum_required(VERSION 3.18)
project(embedding-explorer CXX C)

# Specifications
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/embedding-explorer")

set(SOURCES "${CMAKE_PROJECT_DIR}/explorer.cpp")

add_executable(${PROJECT_NAME} ${SOURCES})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    EXPLORER_DB_PATH="${CMAKE_PROJECT_DIR}/data/main.db"
    EXPLORER_MODEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/models/embeddinggemma-300m/"
)
# System libraries
target_link_libraries(${PROJECT_NAME} PRIVATE ssl crypto pthread)

# EXTERNAL TOOLS

# Include and link LibTorch
set(LOCAL_TORCH "${CMAKE_CURRENT_SOURCE_DIR}/deps/torch")
set(LOCAL_ONEAPI "${CMAKE_CURRENT_SOURCE_DIR}/deps/oneapi")
target_include_directories(${PROJECT_NAME} PRIVATE
    "${LOCAL_TORCH}/include"
    "${LOCAL_TORCH}/include/torch/csrc/api/include")
target_link_directories(${PROJECT_NAME} PRIVATE "${LOCAL_TORCH}/lib")
target_link_libraries(${PROJECT_NAME} PRIVATE torch torch_cpu c10)

# The setup script selects the installed backend from detected hardware.
if(EXISTS "${LOCAL_TORCH}/lib/libtorch_xpu.so")
    target_compile_definitions(${PROJECT_NAME} PRIVATE USE_XPU)
    target_include_directories(${PROJECT_NAME} PRIVATE "${LOCAL_ONEAPI}/include")
    target_link_directories(${PROJECT_NAME} PRIVATE "${LOCAL_ONEAPI}/lib")
    target_link_libraries(${PROJECT_NAME} PRIVATE
        torch_xpu c10_xpu sycl OpenCL pti_view imf
        mkl_sycl_blas
        mkl_sycl_dft
        mkl_sycl_lapack
        mkl_intel_lp64
        mkl_core
        mkl_gnu_thread
    )
    # Let transitive Torch dependencies resolve the local oneAPI libraries.
    target_link_options(${PROJECT_NAME} PRIVATE
        "-Wl,--disable-new-dtags"
        "-Wl,-rpath,${LOCAL_TORCH}/lib:${LOCAL_ONEAPI}/lib"
        "-Wl,-rpath-link,${LOCAL_TORCH}/lib:${LOCAL_ONEAPI}/lib")
endif()

# Include and link nlohmann's json
target_include_directories(${PROJECT_NAME} PRIVATE deps/nlohmann/include)

# Include and link sqlite
add_library(sqlite3 SHARED deps/sqlite3/src/sqlite3.c)
target_include_directories(sqlite3 PUBLIC deps/sqlite3/include)
# Serialized mode is the amalgamation default, pinned because the sync thread
# and the accept loop each hold their own connection to the same file.
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1)
target_link_libraries(${PROJECT_NAME} PRIVATE sqlite3)

# INTERNAL TOOLS

# Include and link fetch/github
add_library(gh.fetch STATIC "${CMAKE_PROJECT_DIR}/fetch/github/src/fetch.c")

add_library(gh.call STATIC "${CMAKE_PROJECT_DIR}/fetch/github/src/call.c") # compile sub libraries
add_library(gh.flush STATIC "${CMAKE_PROJECT_DIR}/fetch/github/src/flush.c")
add_library(gh.parse STATIC "${CMAKE_PROJECT_DIR}/fetch/github/src/parse_http.c")
add_library(gh.sync STATIC "${CMAKE_PROJECT_DIR}/fetch/github/src/sync.c")
add_library(gh.types STATIC "${CMAKE_PROJECT_DIR}/fetch/github/src/types.c")

target_link_libraries(gh.call PRIVATE curl)
target_link_libraries(gh.flush PRIVATE cjson)
target_link_libraries(gh.sync PRIVATE sqlite3)
target_compile_definitions(gh.sync PRIVATE
    EXPLORER_DB_PATH="${CMAKE_PROJECT_DIR}/data/main.db")

target_link_libraries(gh.fetch PRIVATE sqlite3 curl cjson gh.call gh.flush gh.parse gh.sync gh.types) # link sub libraries and include path
target_link_libraries(${PROJECT_NAME} PRIVATE gh.fetch)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_PROJECT_DIR}/fetch/github/include")


# Include and link fetch/devpost
add_library(dp.fetch STATIC "${CMAKE_PROJECT_DIR}/fetch/devpost/src/fetch.c")

add_library(dp.call STATIC "${CMAKE_PROJECT_DIR}/fetch/devpost/src/call.c") # compile sub libraries
add_library(dp.flush STATIC "${CMAKE_PROJECT_DIR}/fetch/devpost/src/flush.c")
add_library(dp.parse STATIC "${CMAKE_PROJECT_DIR}/fetch/devpost/src/parse_html.c")
add_library(dp.sync STATIC "${CMAKE_PROJECT_DIR}/fetch/devpost/src/sync.c")
add_library(dp.types STATIC "${CMAKE_PROJECT_DIR}/fetch/devpost/src/types.c")

target_link_libraries(dp.call PRIVATE curl)
target_link_libraries(dp.flush PRIVATE cjson)
target_link_libraries(dp.sync PRIVATE sqlite3)
target_compile_definitions(dp.sync PRIVATE
    EXPLORER_DB_PATH="${CMAKE_PROJECT_DIR}/data/main.db")

target_link_libraries(dp.fetch PRIVATE sqlite3 curl cjson dp.call dp.flush dp.parse dp.sync dp.types) # link sub libraries and include path
target_link_libraries(${PROJECT_NAME} PRIVATE dp.fetch)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_PROJECT_DIR}/fetch/devpost/include")

# Include modules
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_PROJECT_DIR}/modules/")
