cmake_minimum_required (VERSION 2.7)
project (R-Host)

add_definitions(-DUNICODE -D_UNICODE)

include_directories("${PROJECT_SOURCE_DIR}/../lib/picojson" "${PROJECT_SOURCE_DIR}/../lib/picojson")
link_directories("${PROJECT_SOURCE_DIR}/../lib/picojson")

set(WIN32_LIBRARIES "rstrmgr")

set(R_PATH "C:/Program Files/R/R-3.3.3")
set(R_INCLUDE "${R_PATH}/include")
set(R_LIBRARIES "${R_PATH}/bin/x64/R.dll" "${R_PATH}/bin/x64/Rgraphapp.dll")

#set(Boost_USE_STATIC_LIBS, ON)

find_package(Boost 1.63.0 REQUIRED COMPONENTS date_time filesystem locale program_options regex system)
include_directories(${Boost_INCLUDE_DIRS})

find_library(libzip_LIBRARY REQUIRED NAMES zip)
if (NOT libzip_LIBRARY)
    message(FATAL_ERROR "libzip not found")
endif()
include_directories(${libzip_INCLUDE_DIRS})

find_library(minhook_LIBRARY NAMES MinHook)
if (NOT minhook_LIBRARY)
    message(FATAL_ERROR "minhook not found")
endif()
include_directories(${minhook_INCLUDE_DIRS})

file(GLOB src "*.h" "*.cpp")

add_executable(Microsoft.R.Host ${src})

target_link_libraries(Microsoft.R.Host ${R_LIBRARIES} ${Boost_LIBRARIES} ${libzip_LIBRARY} ${minhook_LIBRARY} ${WIN32_LIBRARIES})
