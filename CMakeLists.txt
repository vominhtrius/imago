cmake_minimum_required(VERSION 3.25)
project(imago LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(USE_JEMALLOC "Link jemalloc as the memory allocator" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR ENABLE_ASAN)
    set(USE_JEMALLOC OFF CACHE BOOL "jemalloc disabled for Debug/ASan builds" FORCE)
endif()

find_package(Drogon CONFIG REQUIRED)
find_package(AWSSDK REQUIRED COMPONENTS core s3)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VIPS REQUIRED IMPORTED_TARGET vips)

set(SOURCES
    src/main.cpp
    src/plugins/S3Plugin.cpp
    src/plugins/VipsPlugin.cpp
    src/plugins/UsecasePlugin.cpp
    src/usecases/ResizeUseCase.cpp
    src/usecases/CropUseCase.cpp
    src/usecases/ConvertUseCase.cpp
    src/usecases/UploadUseCase.cpp
    src/controllers/ResizeController.cpp
    src/controllers/CropController.cpp
    src/controllers/ConvertController.cpp
    src/controllers/UploadController.cpp
    src/infrastructure/s3/S3Client.cpp
    src/infrastructure/vips/VipsImage.cpp
    src/infrastructure/vips/ImageProcessor.cpp
)

add_executable(imago ${SOURCES})

target_include_directories(imago PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AWSSDK_INCLUDE_DIRS}
)

target_link_libraries(imago PRIVATE
    Drogon::Drogon
    ${AWSSDK_LINK_LIBRARIES}
    PkgConfig::VIPS
)

if(USE_JEMALLOC)
    pkg_check_modules(JEMALLOC REQUIRED jemalloc)
    target_link_libraries(imago PRIVATE ${JEMALLOC_LIBRARIES})
    target_include_directories(imago PRIVATE ${JEMALLOC_INCLUDE_DIRS})
    target_compile_definitions(imago PRIVATE USE_JEMALLOC)
    target_compile_options(imago PRIVATE ${JEMALLOC_CFLAGS_OTHER})
endif()

if(ENABLE_ASAN)
    if(APPLE)
        set(ASAN_FLAGS -fsanitize=address)
    else()
        set(ASAN_FLAGS -fsanitize=address,leak)
    endif()
    target_compile_options(imago PRIVATE ${ASAN_FLAGS} -fno-omit-frame-pointer)
    target_link_options(imago PRIVATE ${ASAN_FLAGS})
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(imago PRIVATE IMAGO_DEBUG)
endif()
