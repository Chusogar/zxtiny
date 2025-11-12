cmake_minimum_required(VERSION 3.10)

project(zxtiny
  LANGUAGES C
  DESCRIPTION "A ZX Spectrum emulator written in C99 and SDL2."
  HOMEPAGE_URL "https://github.com/chusogar/zxtiny"
)

set(SOURCES src/main.c src/computers/spectrum/spectrum.c src/cpu/z80/z80.c)
set(ROMS_DIR "./roms/" CACHE STRING "Path to directory containing rom files")

add_executable(zxtiny ${SOURCES})
set_target_properties(zxtiny PROPERTIES C_STANDARD 99)
if (MSVC)
  target_compile_options(zxtiny PRIVATE /W4)
else()
  target_compile_options(zxtiny PRIVATE -Wall -Wextra -pedantic)
endif()

if (EMSCRIPTEN)
  set(CMAKE_EXECUTABLE_SUFFIX ".html")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -s USE_SDL=2")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --preload-file ${ROMS_DIR}@/")
else()
  find_package(SDL2 REQUIRED)
  target_include_directories(zxtiny PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(zxtiny PRIVATE ${SDL2_LIBRARIES})
endif()

target_include_directories(zxtiny PRIVATE src/ roms/)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin" AND NOT EMSCRIPTEN)
  set_target_properties(zxtiny PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "zxtiny"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
    MACOSX_BUNDLE_COPYRIGHT "chusogar"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.chusogar.zxtiny"
  )

  file(COPY ${ROMS_DIR} DESTINATION
    "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.app/Contents/Resources")

  # add NSHighResolutionCapable property in Info.plist for Retina screens
  add_custom_command(TARGET zxtiny POST_BUILD
    COMMAND plutil -replace NSHighResolutionCapable -bool true
    "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.app/Contents/Info.plist"
  )

  # fixup the bundle to make it distributable
  install(CODE "
    include(BundleUtilities)
    set(BU_CHMOD_BUNDLE_ITEMS TRUE)
    fixup_bundle(${CMAKE_BINARY_DIR}/${PROJECT_NAME}.app \"\" \"\")
    " COMPONENT Runtime)
endif()
