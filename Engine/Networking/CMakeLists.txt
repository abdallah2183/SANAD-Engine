# NFNetworking — UDP transport (Phase 16 kickoff).
#
# Datagram sockets only: no threads, no reliability layer yet. Sequence/ack
# replication builds on UdpSocket one layer up.

set(NFNETWORKING_SOURCES
    src/ReliableChannel.cpp
    src/Snapshot.cpp
    src/Authoritative.cpp
    src/PhysicsReplication.cpp
    src/BodyReplication.cpp
    src/ConstraintReplicator.cpp
    # The headless host (design §81): owns the socket and one reliable channel
    # per client around AuthoritativeServer. Deliberately the only entry point
    # that touches Socket_* — the replication classes above stay socket-free.
    src/DedicatedServer.cpp
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
