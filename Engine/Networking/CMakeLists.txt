# NFNetworking — UDP transport (Phase 16 kickoff).
#
# Datagram sockets only: no threads, no reliability layer yet. Sequence/ack
# replication builds on UdpSocket one layer up.

set(NFNETWORKING_SOURCES
    src/ReliableChannel.cpp
    src/Snapshot.cpp
    src/Authoritative.cpp
)

if(WIN32)
    list(APPEND NFNETWORKING_SOURCES src/Socket_Win.cpp)
else()
    list(APPEND NFNETWORKING_SOURCES src/Socket_Stub.cpp)
endif()

nf_engine_library(NFNetworking
    SOURCES ${NFNETWORKING_SOURCES}
    DEPENDS NFCore
    DIR Networking
)

if(WIN32)
    target_link_libraries(NFNetworking PUBLIC ws2_32)
endif()
