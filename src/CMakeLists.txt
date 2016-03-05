add_executable(nulla_server server.cpp)
target_link_libraries(nulla_server
	${AVFORMAT_LIBRARIES}
	${AVCODEC_LIBRARIES}
	${AVUTIL_LIBRARIES}
	${Boost_LIBRARIES}
	${EBUCKET_LIBRARIES}
	${ELLIPTICS_LIBRARIES}
	${GPAC_LIBRARIES}
	${MSGPACK_LIBRARIES}
	${SWARM_LIBRARIES}
	${THEVOID_LIBRARIES}
)

add_executable(nulla_extract_meta extract_meta.cpp)
target_link_libraries(nulla_extract_meta
	${Boost_LIBRARIES}
	${EBUCKET_LIBRARIES}
	${ELLIPTICS_LIBRARIES}
	${GPAC_LIBRARIES}
	${MSGPACK_LIBRARIES}
)
