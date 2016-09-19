project(rtmp-dacast)

find_package(Libcurl REQUIRED)

include_directories(${LIBCURL_INCLUDE_DIRS})
include_directories(${OBS_JANSSON_INCLUDE_DIRS})

set(rtmp-dacast_SOURCES
	rtmp-dacast.c)

set(RTMP_DACAST_API_URL
	"https://api.dacast.com/v2"
	CACHE STRING "Default DaCast API URL")

configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/lookup-config.h.in"
	"${CMAKE_BINARY_DIR}/plugins/rtmp-dacast/lookup-config.h")

set(rtmp-dacast_config_HEADERS
	"${CMAKE_BINARY_DIR}/plugins/rtmp-dacast/lookup-config.h")

add_library(rtmp-dacast MODULE
	${rtmp-dacast_SOURCES}
	${rtmp-dacast_HEADERS}
	${rtmp-dacast_config_HEADERS})

#if(WIN32 AND NOT MINGW)
#	include_directories(../../deps/w32-pthreads)
#	set(rtmp-dacast_PLATFORM_DEPS
#		w32-pthreads)
#endif()

target_link_libraries(rtmp-dacast
	libobs
	${rtmp-dacast_PLATFORM_DEPS}
	${LIBCURL_LIBRARIES}
	${OBS_JANSSON_IMPORT})

target_include_directories(rtmp-dacast
	PUBLIC
		"$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/plugins/rtmp-dacast>")

install_obs_plugin_with_data(rtmp-dacast data)
