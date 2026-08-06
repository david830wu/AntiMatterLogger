# Package smoke test: consumes an INSTALLED AntiMatterLogger via find_package,
# proving the export set, package config, version file, and header install all
# work for a downstream project. Not part of the main build — CI (and you) run:
#
#     cmake --install build --prefix /tmp/amc-prefix
#     cmake -S tests/package -B build-pkg -DCMAKE_PREFIX_PATH=/tmp/amc-prefix
#     cmake --build build-pkg && ./build-pkg/PackageSmoke && ./build-pkg/PackageSmokeCpp

cmake_minimum_required(VERSION 3.20)
project(AmcPackageSmoke LANGUAGES C CXX)

find_package(AntiMatterLogger 0.3 REQUIRED)

add_executable(PackageSmoke PackageSmoke.c)
set_target_properties(PackageSmoke PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(PackageSmoke PRIVATE -Wall -Wextra -Werror)
target_link_libraries(PackageSmoke PRIVATE AntiMatter::AmcLogger)

# The C++ twin: proves the installed header's v2/v3 C++ surface (extern "C" lifecycle +
# C++-capable macros) from a C++ downstream target.
add_executable(PackageSmokeCpp PackageSmokeCpp.cpp)
set_target_properties(PackageSmokeCpp PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(PackageSmokeCpp PRIVATE -Wall -Wextra -Werror)
target_link_libraries(PackageSmokeCpp PRIVATE AntiMatter::AmcLogger)
