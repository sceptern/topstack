cmake_minimum_required(VERSION 3.21)
project(topstack VERSION 0.1.0 LANGUAGES CXX)

# Header-only: other projects use it with
#   add_subdirectory(path/to/topstack)
#   target_link_libraries(my_app PRIVATE topstack::topstack)
add_library(topstack INTERFACE)
add_library(topstack::topstack ALIAS topstack)
target_include_directories(topstack INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(topstack INTERFACE cxx_std_20)

# The tests are built only when topstack is the top-level project (or when asked).
option(TOPSTACK_BUILD_TESTS "Build topstack's tests" ${PROJECT_IS_TOP_LEVEL})
if(TOPSTACK_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)
    foreach(t stack_test grow_buf_test stack_set_test ref_test ref16_test plain_only global_buf)
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE topstack Threads::Threads)
        if(NOT MSVC)
            target_compile_options(${t} PRIVATE -Wall -Wextra)
        endif()
        add_test(NAME ${t} COMMAND ${t})
    endforeach()
endif()
