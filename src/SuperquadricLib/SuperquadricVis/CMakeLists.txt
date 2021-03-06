#Copyright (C) 2019 Istituto Italiano di Tecnologia (IIT)
#Author: Giulia Vezzani <giulia.vezzani@iit.it>

#This library is free software; you can redistribute it and/or
#modify it under the terms of the GNU Lesser General Public
#License as published by the Free Software Foundation; either
#version 2.1 of the License, or (at your option) any later version.

#This library is distributed in the hope that it will be useful,
#but WITHOUT ANY WARRANTY; without even the implied warranty of
#MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#Lesser General Public License for more details.

#You should have received a copy of the GNU Lesser General Public
#License along with this library; if not, write to the Free Software
#Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

# This is the "physical" (i.e. real) name of the library.
# The actual file name of the library built is constructed based on conventions
# of the native platform, such as lib<LIBRARY_TARGET_NAME>.a,
# lib<LIBRARY_TARGET_NAME>.dylib or <LIBRARY_TARGET_NAME>.lib.
#
# In order to use a library name that is not the same as the `EXPORT` (in the
# `install` command), it is necessary to pass the `FIRST_TARGET` option to
# `install_basic_package_files` in the main CMakeLists.txt file.
# In this case the project name is used for both, therefore it is not necessary.
set(LIBRARY_TARGET_NAME SuperquadricLibVis)

# List of CPP (source) library files.
set(${LIBRARY_TARGET_NAME}_SRC
		src/vis.cpp
		src/visRenderer.cpp
        src/poseVis.cpp
		src/planeVis.cpp
		src/pointsVis.cpp
		src/superqVis.cpp
)

# List of HPP (header) library files.
set(${LIBRARY_TARGET_NAME}_HDR
		include/SuperquadricLibVis/vis.h
		include/SuperquadricLibVis/visRenderer.h
        include/SuperquadricLibVis/poseVis.h
		include/SuperquadricLibVis/planeVis.h
		include/SuperquadricLibVis/pointsVis.h
		include/SuperquadricLibVis/superqVis.h
)

find_package(VTK REQUIRED)
#if(VTK_VERSION VERSION_LESS "8.1")
#	message(ERROR " Wrong VTK version! At least 8.1 required!")
#endif()

include(${VTK_USE_FILE})

find_package(Eigen3 QUIET CONFIG)
if(NOT EIGEN3_FOUND)
    find_package(Eigen3 REQUIRED)
endif()

# You can add an external dependency using the find_package() function call
# See: https://cmake.org/cmake/help/latest/command/find_package.html
# Note that the imported objects resulting from the find_package() depends upon
# the configuration files generated by the developer of the library.
# These imported objects can be either:
#  - variables that must be used to include directories and/or link libraries
#    with function calls like target_include_directories() and/or
#    target_link_libraries();
#  - a CMake target that must only be linked using target_link_libraries() and
#    CMake will take care of including and/or linking the appropriate
#    directories and/or libraries.
# See: https://cmake.org/cmake/help/latest/command/target_include_directories.html
# See: https://cmake.org/cmake/help/latest/command/target_link_libraries.html
# For example:
# find_package(FooPackage) may import either
#  - variables: FooPackage_INCLUDE_DIRS and FooPackage_LIBRARIES
#  - target: FooPackage::FooPackage

# Adds a library target called ${LIBRARY_TARGET_NAME} to be built from the
# source and header files listed in the command invocation.
add_library(${LIBRARY_TARGET_NAME} ${${LIBRARY_TARGET_NAME}_SRC} ${${LIBRARY_TARGET_NAME}_HDR})

# Set two minimum target properties for the library.
# See https://cmake.org/cmake/help/latest/command/set_target_properties.html
# Properties are: 1) Library version number 2) list of corresponding public headers
set_target_properties(${LIBRARY_TARGET_NAME} PROPERTIES VERSION       ${${PROJECT_NAME}_VERSION}
                                                        PUBLIC_HEADER "${${LIBRARY_TARGET_NAME}_HDR}")

# Specify include directories for both compilation and installation process.
# The $<INSTALL_PREFIX> generator expression is useful to ensure to create
# relocatable configuration files, see https://cmake.org/cmake/help/latest/manual/cmake-packages.7.html#creating-relocatable-packages
target_include_directories(${LIBRARY_TARGET_NAME} PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                                                         "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")

target_link_libraries(${LIBRARY_TARGET_NAME} PUBLIC ${VTK_LIBRARIES} SuperquadricLibModel SuperquadricLibGrasp)
target_include_directories(${LIBRARY_TARGET_NAME} PUBLIC ${VTK_INCLUDE_DIRS})

if(NOT TARGET Eigen3)
    target_include_directories(${LIBRARY_TARGET_NAME} PUBLIC ${EIGEN3_INCLUDE_DIR})

	target_link_libraries(${LIBRARY_TARGET_NAME} PUBLIC ${CMAKE_THREAD_LIBS_INIT})
else()
    target_link_libraries(${LIBRARY_TARGET_NAME} PUBLIC Eigen3::Eigen)
endif()

# If you used find_package() you need to use target_include_directories() and/or
# target_link_libraries(). As explained previously, depending on the imported
# objects, you may need to call either or both:
# - with imported variable:
#   target_include_directories(${LIBRARY_TARGET_NAME} ${FooPackage_INCLUDE_DIRS})
#   target_link_libraries(${LIBRARY_TARGET_NAME} ${FooPackage_LIBRARIES})
# - with imported target:
#   target_link_libraries(${LIBRARY_TARGET_NAME} FooPackage_LIBRARIES::FooPackage_LIBRARIES)

#set(${PROJECT_NAME}_TARGETS ${${PROJECT_NAME}_TARGETS} ${LIBRARY_TARGET_NAME} PARENT_SCOPE)


# Specify installation targets, typology and destination folders.
install(TARGETS ${LIBRARY_TARGET_NAME}
        EXPORT  ${PROJECT_NAME}
        LIBRARY       DESTINATION "${CMAKE_INSTALL_LIBDIR}"                            COMPONENT shlib
        ARCHIVE       DESTINATION "${CMAKE_INSTALL_LIBDIR}"                            COMPONENT lib
        RUNTIME       DESTINATION "${CMAKE_INSTALL_BINDIR}"                            COMPONENT bin
        PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${LIBRARY_TARGET_NAME}" COMPONENT dev)
