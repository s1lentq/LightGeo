cmake_minimum_required(VERSION 3.16)

project(lightgeo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
	add_compile_options(/utf-8 /W4)
endif()

option(LIGHTGEO_BUILD_BENCHMARK "Build benchmarks and MaxMind comparison" ON)
option(LIGHTGEO_BUILD_STATIC_RUNTIME "Link with static runtime (/MT or /MTd)" ON)

if(MSVC)
	if(LIGHTGEO_BUILD_STATIC_RUNTIME)
		set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	else()
		set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<NOT:$<CONFIG:Debug>>:DLL>$<$<CONFIG:Debug>:DebugDLL>")
	endif()
endif()

add_executable(lightgeo src/main.cpp)

target_include_directories(lightgeo PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(LIGHTGEO_BUILD_BENCHMARK)
	include(FetchContent)
	FetchContent_Declare(
		libmaxminddb
		GIT_REPOSITORY https://github.com/maxmind/libmaxminddb.git
		GIT_TAG main
		PATCH_COMMAND ${CMAKE_COMMAND} -E make_directory man # suppress man generation
	)

	set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
	set(MAXMINDDB_INSTALL OFF CACHE BOOL "" FORCE)
	set(MAXMINDDB_BUILD_BINARIES OFF CACHE BOOL "" FORCE)
	FetchContent_MakeAvailable(libmaxminddb)

	add_executable(lightgeo_bench tests/benchmark.cpp)

	target_include_directories(lightgeo_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
	target_link_libraries(lightgeo_bench PRIVATE maxminddb)
	target_compile_definitions(lightgeo_bench PRIVATE USE_MAXMINDDB)
endif()
