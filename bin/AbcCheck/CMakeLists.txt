ADD_EXECUTABLE(abcvalidate AbcCheck.cpp)
TARGET_LINK_LIBRARIES(abcvalidate Alembic::Alembic)
INSTALL(TARGETS abcvalidate DESTINATION bin)
