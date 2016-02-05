add_executable(nulla_server server.cpp)
target_link_libraries(nulla_server
	${Boost_LIBRARIES}
	${MSGPACK_LIBRARIES}
	${THEVOID_LIBRARIES}
	${SWARM_LIBRARIES}
)
