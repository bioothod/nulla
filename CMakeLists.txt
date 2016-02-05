project(nulla)
cmake_minimum_required(VERSION 2.6)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x -W -Wall -Wextra -fstack-protector-all")

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/debian/changelog" DEBCHANGELOG)

# version string: 2.26.0.3.12
# elliptics version is 2.26
# nulla major 2.26.0
# nulla minor 3.12
string(REGEX MATCH "([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)" DEBFULLVERSION "${DEBCHANGELOG}")
string(REGEX MATCH "([0-9]+\\.[0-9]+)" NULLA_VERSION_ELLIPTICS "${DEBFULLVERSION}")
string(REGEX MATCH "([0-9]+\\.[0-9]+\\.[0-9]+)" NULLA_VERSION_MAJOR "${DEBFULLVERSION}")
string(REGEX MATCH "([0-9]+\\.[0-9]+$)" NULLA_VERSION_MINOR "${DEBFULLVERSION}")

include(cmake/locate_library.cmake)

LOCATE_LIBRARY(THEVOID "thevoid/server.hpp" "thevoid")
LOCATE_LIBRARY(SWARM "swarm/http_response.hpp" "swarm")
LOCATE_LIBRARY(MSGPACK "msgpack.hpp" "msgpack")

find_package(Boost COMPONENTS system thread REQUIRED)
find_package(Elliptics ${RIFT_VERSION_ELLIPTICS} REQUIRED)

include_directories(
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	${Boost_INCLUDE_DIRS}
	${ELLIPTICS_INCLUDE_DIRS}
	${MSGPACK_INCLUDE_DIRS}
	${SWARM_INCLUDE_DIRS}
	${THEVOID_INCLUDE_DIRS}
)

link_directories(
	${Boost_LIBRARY_DIRS}
	${ELLIPTICS_LIBRARY_DIRS}
	${MSGPACK_LIBRARY_DIRS}
	${THEVOID_LIBRARY_DIRS}
)

add_subdirectory(src)
#add_subdirectory(tests)

file(GLOB headers "${CMAKE_CURRENT_SOURCE_DIR}/include/nulla/*.hpp")
install(FILES ${headers} DESTINATION include/nulla)
