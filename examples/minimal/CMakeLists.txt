cmake_minimum_required(VERSION 3.21)
project(agent_framework_minimal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC OFF)

find_package(AgentFramework 0.2 REQUIRED)

add_executable(agent_framework_minimal main.cpp)
target_link_libraries(agent_framework_minimal PRIVATE AgentFramework::agent_framework)
