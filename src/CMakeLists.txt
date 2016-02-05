add_executable(nulla_server server.cpp)
target_link_libraries(nulla_server
	${Boost_LIBRARIES}
	${ELLIPTICS_LIBRARIES}
	${MSGPACK_LIBRARIES}
	${SWARM_LIBRARIES}
	${THEVOID_LIBRARIES}
)
