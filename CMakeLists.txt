cmake_minimum_required(VERSION 3.16)
project(tabler-server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)

add_executable(tabler-server
  src/main.cpp
  src/config.cpp
  src/http.cpp
  src/worker.cpp
  src/proxy.cpp
)

target_link_libraries(tabler-server PRIVATE Threads::Threads)

install(TARGETS tabler-server RUNTIME DESTINATION bin)
install(FILES R/worker.R DESTINATION share/tabler-server)
