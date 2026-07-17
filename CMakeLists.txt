cmake_minimum_required(VERSION 4.1)
project(
    gca
    DESCRIPTION "A toy allocator with garbage collection that lacks speed, but supports is thread safe at the very least"
    LANGUAGES CXX
    VERSION 0.0.0
)

set(CMAKE_CXX_STANDARD 20)

add_library(gca)
add_library(gca::gca ALIAS gca)

target_sources(gca PRIVATE gc.cpp)
target_sources(
    gca
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
        FILES
        include/gca/gc-config.h
        include/gca/gc-exceptions.h
        include/gca/gc-util.h
        include/gca/gc.h
)

include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/52eb8108c5bdec04579160ae17225d66034bd723.zip # 1.17.0
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(CTest)
enable_testing()

add_executable(gca.tests)
target_sources(gca.tests PRIVATE tests/gc-test.cpp)
target_link_libraries(gca.tests PRIVATE gca::gca GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main)
include(GoogleTest)
gtest_discover_tests(gca.tests)

set_target_properties(
    gca
    PROPERTIES
        VERIFY_INTERFACE_HEADER_SETS ON
        EXPORT_NAME gca
)

include(GNUInstallDirs)
install(
    TARGETS gca COMPONENT gca
    EXPORT gca
    DESTINATION ${CMAKE_INSTALL_LIBDIR}$<$<CONFIG:Debug>:/debug>
    FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/gca-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/gca-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gca
    PATH_VARS PROJECT_NAME PROJECT_VERSION
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/gca-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY ExactVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/gca-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/gca-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gca
    COMPONENT gca
)
install(
    EXPORT gca
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gca
    NAMESPACE gca::
    FILE gca-targets.cmake
    COMPONENT gca
)