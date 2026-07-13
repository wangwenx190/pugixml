cmake_minimum_required(VERSION 4.0)

project(pugixml
    VERSION 1.16.0
    DESCRIPTION "Light-weight C++ XML processing library"
    LANGUAGES CXX
)

if(WIN32)
    enable_language(RC)
endif()
if(NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_CXX_VISIBILITY_PRESET "hidden")
if(NOT "x${CMAKE_CXX_FLAGS}" STREQUAL "x")
    if(MSVC)
        string(REGEX REPLACE " [/-]W[01234] " " " CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
    else()
        string(REGEX REPLACE " -W(all)?(extra)? " " " CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
        string(REGEX REPLACE " -[W]?pedantic " " " CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
    endif()
endif()
string(APPEND CMAKE_CXX_FLAGS " -w ")

set(__top_level ON)
if("x${CMAKE_SOURCE_DIR}" STREQUAL "x${PROJECT_SOURCE_DIR}")
    message("pugixml: Building as a top-level project.")
else()
    set(__top_level OFF)
    message("pugixml: Building as a subproject.")
endif()

option(PUGIXML_BUILD_SHARED "Build shared library edition of pugixml." ON)
option(PUGIXML_BUILD_STATIC "Build static library edition of pugixml." ON)
option(PUGIXML_INSTALL "Enable installation for pugixml." OFF)
if(NOT PUGIXML_BUILD_SHARED AND NOT PUGIXML_BUILD_STATIC)
    message(SEND_ERROR "You MUST build either the shared library or the static library or both of them!")
endif()

set(__targets)
if(PUGIXML_BUILD_SHARED)
    add_library(${PROJECT_NAME}_shared SHARED)
    add_library(${PROJECT_NAME}::${PROJECT_NAME}_shared ALIAS ${PROJECT_NAME}_shared)
    set_target_properties(${PROJECT_NAME}_shared PROPERTIES OUTPUT_NAME ${PROJECT_NAME})
    target_compile_definitions(${PROJECT_NAME}_shared PRIVATE PUGIXML_EXPORTS)
    if(WIN32)
        target_sources(${PROJECT_NAME}_shared PRIVATE ${PROJECT_NAME}.rc)
    endif()
    list(APPEND __targets ${PROJECT_NAME}_shared)
endif()
if(PUGIXML_BUILD_STATIC)
    add_library(${PROJECT_NAME}_static STATIC)
    add_library(${PROJECT_NAME}::${PROJECT_NAME}_static ALIAS ${PROJECT_NAME}_static)
    target_compile_definitions(${PROJECT_NAME}_static PUBLIC PUGIXML_STATIC_DEFINE)
    list(APPEND __targets ${PROJECT_NAME}_static)
endif()
if(PUGIXML_BUILD_SHARED AND NOT PUGIXML_BUILD_STATIC)
    add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME}_shared)
endif()
if(PUGIXML_BUILD_STATIC AND NOT PUGIXML_BUILD_SHARED)
    add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME}_static)
endif()

include(GNUInstallDirs)
file(GLOB_RECURSE __sources LIST_DIRECTORIES false CONFIGURE_DEPENDS
    "./src/*.hpp" "./src/*.cpp"
)
foreach(__target IN LISTS __targets)
    target_sources(${__target} PRIVATE ${__sources})
    target_compile_features(${__target} PUBLIC cxx_std_23)
    target_include_directories(${__target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    if(__top_level)
        if(WIN32)
            target_compile_definitions(${__target} PRIVATE
                UNICODE _UNICODE
                NOMINMAX
                WIN32_LEAN_AND_MEAN
            )
        endif()
        if(MSVC)
            target_compile_options(${__target} PRIVATE
                /utf-8 /permissive- /Zc:__cplusplus
                $<$<NOT:$<CONFIG:Debug>>:/Gw /Gy /Zc:inline>
            )
            target_link_options(${__target} PRIVATE
                $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>
            )
        else()
            target_compile_options(${__target} PRIVATE
                $<$<NOT:$<CONFIG:Debug>>:-ffunction-sections -fdata-sections>
            )
            if(WIN32)
                target_link_options(${__target} PRIVATE
                    $<$<NOT:$<CONFIG:Debug>>:-Wl,/OPT:REF,/OPT:ICF>
                )
            elseif(APPLE)
                target_link_options(${__target} PRIVATE
                    $<$<NOT:$<CONFIG:Debug>>:-Wl,-dead_strip>
                )
            else()
                target_link_options(${__target} PRIVATE
                    $<$<NOT:$<CONFIG:Debug>>:-Wl,--gc-sections>
                )
            endif()
        endif()
    endif()
    set_target_properties(${__target} PROPERTIES VERSION ${PROJECT_VERSION})
    get_target_property(__type ${__target} TYPE)
    if(NOT "x${__type}" STREQUAL "xSTATIC_LIBRARY")
        set_target_properties(${__target} PROPERTIES SOVERSION ${PROJECT_VERSION_MAJOR})
    endif()
    if(PUGIXML_INSTALL)
        install(TARGETS ${__target}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()
endforeach()

if(PUGIXML_INSTALL)
    install(
        FILES src/pugixml.hpp src/pugiconfig.hpp src/pugixml_dom.hpp src/pugixml_sax.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

option(PUGIXML_BUILD_TESTS "Build tests and benchmarks." OFF)
if(PUGIXML_BUILD_TESTS)
    enable_testing()
    list(GET __targets 0 __test_library)

    add_executable(pugixml_dom_view_test tests/dom_view.cpp)
    target_link_libraries(pugixml_dom_view_test PRIVATE ${__test_library})
    add_test(NAME pugixml_dom_view COMMAND pugixml_dom_view_test)

    add_executable(pugixml_dom_view_benchmark tests/dom_view_benchmark.cpp)
    target_link_libraries(pugixml_dom_view_benchmark PRIVATE ${__test_library})

    add_executable(pugixml_sax_test tests/sax.cpp)
    target_link_libraries(pugixml_sax_test PRIVATE ${__test_library})
    add_test(NAME pugixml_sax COMMAND pugixml_sax_test)

    add_executable(pugixml_sax_benchmark tests/sax_benchmark.cpp)
    target_link_libraries(pugixml_sax_benchmark PRIVATE ${__test_library})
endif()
