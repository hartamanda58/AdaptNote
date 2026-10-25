cmake_minimum_required(VERSION 3.21)
project(AdaptNote VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Sql Network)
find_package(Qt6 QUIET COMPONENTS TextToSpeech)

# ---------------------------------------------------------------------------
# MuPDF
#   Linux:  apt install libmupdf-dev  (static libmupdf.a + libmupdf-third.a)
#   Windows: build MuPDF with `nmake HAVE_X11=no HAVE_GLUT=no` (or the official
#            prebuilt) and point -DMUPDF_ROOT=<dir> at it. Expect a static
#            libmupdf.lib built with the same MSVC runtime as Qt.
# ---------------------------------------------------------------------------
if(WIN32)
  set(MUPDF_ROOT "" CACHE PATH "MuPDF source/build root (contains include/ and platform/win32)")
  find_path(MUPDF_INCLUDE_DIR mupdf/fitz.h
            PATHS ${MUPDF_ROOT}/include ${MUPDF_ROOT} "${CMAKE_SOURCE_DIR}/third_party/mupdf/include")
  find_library(MUPDF_LIB NAMES mupdf libmupdf
               PATHS ${MUPDF_ROOT}/platform/win32/x64/Release
                     ${MUPDF_ROOT}/build/release ${MUPDF_ROOT}/lib
                     "${CMAKE_SOURCE_DIR}/third_party/mupdf/lib")
  # The MSVC solution emits several static libs (mupdf / thirdparty / muthreads /
  # resources); link them all, in this order, plus the Win32 system libs they need.
  file(GLOB MUPDF_WIN_EXTRA
       "${MUPDF_ROOT}/platform/win32/x64/Release/libthirdparty.lib"
       "${MUPDF_ROOT}/platform/win32/x64/Release/libmuthreads.lib"
       "${MUPDF_ROOT}/platform/win32/x64/Release/libresources.lib")
  if(NOT MUPDF_INCLUDE_DIR OR NOT MUPDF_LIB)
    message(FATAL_ERROR
      "MuPDF not found. Build it (nmake HAVE_X11=no) and pass -DMUPDF_ROOT=<dir>, "
      "or drop it into third_party/mupdf/.")
  endif()
  # src/export/zip.cpp needs zlib.h; MuPDF vendors zlib and libthirdparty already
  # provides crc32/deflate/inflate, so reuse its headers instead of another dependency.
  file(GLOB ZLIB_INCLUDE_CANDIDATES
       "${MUPDF_ROOT}/thirdparty/zlib"
       "${MUPDF_ROOT}/thirdparty/zlib/include")
  foreach(cand ${ZLIB_INCLUDE_CANDIDATES})
    if(EXISTS "${cand}/zlib.h")
      set(ZLIB_INCLUDE_DIR "${cand}")
      break()
    endif()
  endforeach()
  if(NOT ZLIB_INCLUDE_DIR)
    file(GLOB_RECURSE _zlib_h "${MUPDF_ROOT}/thirdparty/*/zlib.h")
    if(_zlib_h)
      get_filename_component(ZLIB_INCLUDE_DIR "${_zlib_h}" DIRECTORY)
    endif()
  endif()
  if(ZLIB_INCLUDE_DIR)
    message(STATUS "zlib headers: ${ZLIB_INCLUDE_DIR} (from MuPDF)")
    set(MUPDF_ZLIB_INCLUDE ${ZLIB_INCLUDE_DIR})
  else()
    message(WARNING "zlib.h not found under ${MUPDF_ROOT}: ZIP export will not build")
  endif()

  set(MUPDF_LIBS ${MUPDF_LIB} ${MUPDF_WIN_EXTRA}
      user32 gdi32 advapi32 ole32 shell32 ws2_32 comdlg32)
  message(STATUS "MuPDF: ${MUPDF_LIB} (+ ${MUPDF_WIN_EXTRA})")
else()
  find_path(MUPDF_INCLUDE_DIR mupdf/fitz.h PATHS /usr/include)
  find_library(MUPDF_LIB mupdf PATHS /usr/lib)
  find_library(MUPDF_THIRD_LIB mupdf-third PATHS /usr/lib)
  if(NOT MUPDF_INCLUDE_DIR OR NOT MUPDF_LIB OR NOT MUPDF_THIRD_LIB)
    message(FATAL_ERROR "MuPDF dev files not found (apt install libmupdf-dev)")
  endif()
  set(MUPDF_LIBS ${MUPDF_LIB} ${MUPDF_THIRD_LIB}
      mujs freetype harfbuzz gumbo openjp2 jbig2dec jpeg z m pthread)
  message(STATUS "MuPDF: ${MUPDF_LIB} + ${MUPDF_THIRD_LIB}")
endif()

# ---- core library: model + store + engine (no GUI) ----
add_library(adapt_core STATIC
  src/core/model.cpp
  src/core/store.cpp
  src/core/textselect.cpp
  src/sync/vv.cpp
  src/ai/ai_client.cpp
  src/ai/prompts.cpp
  src/mind/tree.cpp
  src/migrate/migrate.cpp
  src/import/import.cpp
  src/sync/conflicts.cpp
  src/ink/ink.cpp
  src/read/reader.cpp
  src/sync/webdav_client.cpp
  src/sync/sync_manager.cpp
  src/render/mupdf_engine.cpp
  src/export/zip.cpp
  src/export/exporter.cpp
)
target_include_directories(adapt_core PUBLIC src ${MUPDF_INCLUDE_DIR})
if(MUPDF_ZLIB_INCLUDE)
  target_include_directories(adapt_core PRIVATE ${MUPDF_ZLIB_INCLUDE})
endif()
target_link_libraries(adapt_core PUBLIC Qt6::Core Qt6::Gui Qt6::Sql Qt6::Network)
target_link_libraries(adapt_core PUBLIC ${MUPDF_LIBS})

if(MSVC)
  # MuPDF's headers are C; keep the console quiet about warning noise from them.
  target_compile_options(adapt_core PRIVATE /W3 /permissive- /utf-8 /MP)
  target_compile_definitions(adapt_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
  # static runtime must match the MuPDF build; keep the default dynamic CRT.
  set_property(TARGET adapt_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
else()
  target_compile_options(adapt_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(WIN32)
  # `cmake --build . --target deploy-windows` stages a runnable folder next to the exe.
  find_program(WINDEPLOYQT_EXE windeployqt HINTS ${QT6_INSTALL_PREFIX}/bin "${QT_BIN_DIR}")
  if(WINDEPLOYQT_EXE)
    add_custom_target(deploy-windows
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/deploy
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:adaptnote> ${CMAKE_BINARY_DIR}/deploy/
      COMMAND ${WINDEPLOYQT_EXE} --release --no-translations --no-system-d3d-compiler
              --no-opengl-sw ${CMAKE_BINARY_DIR}/deploy/adaptnote.exe
      DEPENDS adaptnote
      COMMENT "Staging a portable folder with windeployqt")
  endif()
endif()

if(Qt6TextToSpeech_FOUND)
  target_link_libraries(adapt_core PUBLIC Qt6::TextToSpeech)
  target_compile_definitions(adapt_core PUBLIC ADAPT_HAVE_TTS)
  message(STATUS "TTS: enabled (Qt6::TextToSpeech)")
else()
  message(STATUS "TTS: unavailable -> read-aloud uses timed pacing")
endif()

# ---- ui library ----
add_library(adapt_ui STATIC
  src/ui/mainwindow.cpp
  src/ui/pageview.cpp
  src/ui/libraryview.cpp
  src/ui/conflictdialog.cpp
  src/mind/mindmap.cpp
)
target_link_libraries(adapt_ui PUBLIC adapt_core Qt6::Widgets Qt6::Gui)
qt_add_resources(adapt_ui "appres"
  PREFIX "/"
  FILES resources/app.qss
)

# ---- application ----
add_executable(adaptnote src/main.cpp)
target_link_libraries(adaptnote PRIVATE adapt_ui)

# ---- tools: test document generator ----
add_executable(mkpdf tools/mkpdf.cpp)
target_link_libraries(mkpdf PRIVATE Qt6::Gui)

add_executable(bench tools/bench.cpp)
target_link_libraries(bench PRIVATE adapt_core Qt6::Gui)

# ---- tests ----
add_executable(selftest tests/selftest.cpp)
target_link_libraries(selftest PRIVATE adapt_core)

add_executable(visualtest tests/visualtest.cpp)
target_link_libraries(visualtest PRIVATE adapt_ui)

add_executable(featuretest tests/featuretest.cpp)
target_link_libraries(featuretest PRIVATE adapt_ui)

add_executable(exporttest tests/exporttest.cpp)
target_link_libraries(exporttest PRIVATE adapt_core)

add_executable(synctest tests/synctest.cpp)
target_link_libraries(synctest PRIVATE adapt_core)

add_executable(aitest tests/aitest.cpp)
target_link_libraries(aitest PRIVATE adapt_core)

add_executable(p5test tests/p5test.cpp)
target_link_libraries(p5test PRIVATE adapt_ui)

add_executable(librarytest tests/librarytest.cpp)
target_link_libraries(librarytest PRIVATE adapt_ui)

add_executable(migratetest tests/migratetest.cpp)
target_link_libraries(migratetest PRIVATE adapt_core)

add_executable(importtest tests/importtest.cpp)
target_link_libraries(importtest PRIVATE adapt_core)

add_executable(conflicttest tests/conflicttest.cpp)
target_link_libraries(conflicttest PRIVATE adapt_ui)

enable_testing()
add_test(NAME selftest COMMAND selftest test.pdf test.epub WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME visualtest COMMAND visualtest test.pdf WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME featuretest COMMAND featuretest test.pdf WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME exporttest COMMAND exporttest test.pdf WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME synctest COMMAND synctest test.pdf ${CMAKE_SOURCE_DIR}/scripts/dav_server.py WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME aitest COMMAND aitest ${CMAKE_SOURCE_DIR}/scripts/fake_openai_server.py WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME p5test COMMAND p5test test.pdf WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME librarytest COMMAND librarytest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME migratetest COMMAND migratetest test.pdf WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME importtest COMMAND importtest test.pdf WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME conflicttest COMMAND conflicttest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(selftest visualtest featuretest exporttest synctest aitest p5test librarytest
                     migratetest importtest conflicttest
                     PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=${CMAKE_BINARY_DIR}/testhome")
