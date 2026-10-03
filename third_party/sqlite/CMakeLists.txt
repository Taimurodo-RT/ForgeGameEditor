# SQLite 3.53.4 amalgamation (public domain), generated from the official
# source tag version-3.53.4. Vendored because building the amalgamation needs
# Tcl, which Windows machines usually do not have.
add_library(sqlite3 STATIC sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE
  SQLITE_THREADSAFE=2          # one connection per thread
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  SQLITE_ENABLE_FTS5           # full-text search over asset names, paths and tags
  SQLITE_DQS=0
  SQLITE_OMIT_DEPRECATED
  SQLITE_OMIT_LOAD_EXTENSION)
if(NOT MSVC)
  target_compile_options(sqlite3 PRIVATE -w)
  find_package(Threads REQUIRED)
  target_link_libraries(sqlite3 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()
