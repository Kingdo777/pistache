include_directories(../include)
link_libraries(pistache pthread)
add_executable(http_server http_server.cc)
add_executable(hello_server hello_server.cc)
add_executable(test test.cpp)
add_executable(server server.cpp)
add_executable(client http_client.cc)