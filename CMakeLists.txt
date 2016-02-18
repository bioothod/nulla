project(nulla)
cmake_minimum_required(VERSION 2.6)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -W -Wall -Wextra -fstack-protector-all -g")

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/debian/changelog" DEBCHANGELOG)

# version string: 0.3.12
# nulla major 0
# nulla minor 3.12
string(REGEX MATCH "([0-9]+\\.[0-9]+\\.[0-9]+)" DEBFULLVERSION "${DEBCHANGELOG}")
string(REGEX MATCH "([0-9]+)" NULLA_VERSION_MAJOR "${DEBFULLVERSION}")
string(REGEX MATCH "([0-9]+\\.[0-9]+$)" NULLA_VERSION_MINOR "${DEBFULLVERSION}")

include(cmake/locate_library.cmake)

LOCATE_LIBRARY(THEVOID "thevoid/server.hpp" "thevoid")
LOCATE_LIBRARY(SWARM "swarm/http_response.hpp" "swarm")
LOCATE_LIBRARY(MSGPACK "msgpack.hpp" "msgpack")
LOCATE_LIBRARY(GPAC "gpac/isomedia.h" "gpac")
LOCATE_HEADERS(EBUCKET "ebucket/bucket.hpp")

find_package(Boost COMPONENTS system thread filesystem program_options REQUIRED)
find_package(Elliptics REQUIRED)

include_directories(
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	${Boost_INCLUDE_DIRS}
	${EBUCKET_INCLUDE_DIRS}
	${ELLIPTICS_INCLUDE_DIRS}
	${GPAC_INCLUDE_DIRS}
	${MSGPACK_INCLUDE_DIRS}
	${SWARM_INCLUDE_DIRS}
	${THEVOID_INCLUDE_DIRS}
)

link_directories(
	${Boost_LIBRARY_DIRS}
	${EBUCKET_LIBRARY_DIRS}
	${ELLIPTICS_LIBRARY_DIRS}
	${GPAC_LIBRARY_DIRS}
	${MSGPACK_LIBRARY_DIRS}
	${THEVOID_LIBRARY_DIRS}
)

add_subdirectory(src)
#add_subdirectory(tests)

file(GLOB headers "${CMAKE_CURRENT_SOURCE_DIR}/include/nulla/*.hpp")
install(FILES ${headers} DESTINATION include/nulla)
