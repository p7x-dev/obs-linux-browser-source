cmake_minimum_required(VERSION 3.21)

project(obs-widget-browser
        VERSION 0.1.0
        DESCRIPTION "Browser widget source for OBS Studio without CEF (Qt WebEngine based)"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(libobs REQUIRED)
find_package(Qt6 6.4 REQUIRED COMPONENTS
    Core Gui Quick Network WebChannel WebChannelQuick
    WebEngineCore WebEngineQuick)

add_library(obs-widget-browser MODULE
    src/plugin.cpp
    src/widget-source.cpp
    src/widget-renderer.cpp
    src/url-utils.cpp
)

qt_add_resources(obs-widget-browser "qml"
    PREFIX "/qml"
    BASE "src/qml"
    FILES
        src/qml/WidgetView.qml
        src/qml/qwebchannel.js
        src/qml/discovery.js
)

target_include_directories(obs-widget-browser PRIVATE src)

# Arch Linux ships the QRhi private headers under a versioned subdirectory
# (QtGui/<ver>/QtGui/rhi/qrhi.h); other distros expose them as <rhi/qrhi.h>.
foreach(dir
        /usr/include/qt6/QtGui/6.11.2/QtGui
        /usr/include/qt6/QtGui/6.11.2)
    if(EXISTS "${dir}/rhi/qrhi.h")
        target_include_directories(obs-widget-browser PRIVATE "${dir}")
    endif()
endforeach()
target_link_libraries(obs-widget-browser PRIVATE
    OBS::libobs
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Network
    Qt6::WebChannel
    Qt6::WebChannelQuick
    Qt6::WebEngineCore
    Qt6::WebEngineQuick
)

set_target_properties(obs-widget-browser PROPERTIES
    PREFIX ""
    OUTPUT_NAME "obs-widget-browser"
)

include(GNUInstallDirs)

install(TARGETS obs-widget-browser
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins/obs-widget-browser/bin/64bit")
install(FILES data/locale/en-US.ini data/locale/ru-RU.ini
        DESTINATION "${CMAKE_INSTALL_DATADIR}/obs/obs-plugins/obs-widget-browser/data/locale")

enable_testing()
add_executable(url-utils-tests
    tests/test_url_utils.cpp
    src/url-utils.cpp
)
target_include_directories(url-utils-tests PRIVATE src)
add_test(NAME url_utils COMMAND url-utils-tests)