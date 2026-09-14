# NFJobs — Job system, scheduler, work stealing

set(NFJOBS_SOURCES
    src/JobSystem.cpp
    src/JobScheduler.cpp
    src/WorkerThread.cpp
)

nf_engine_library(NFJobs
    SOURCES ${NFJOBS_SOURCES}
    DEPENDS NFCore
)
