project(ssp-connector)

include(FetchContent)
FetchContent_Declare(
  libssp
  GIT_REPOSITORY "https://github.com/imaginevision/libssp.git"
  GIT_TAG f9882aeebdda781d14262fcc04b9038a567ac985)

if(NOT libssp_POPULATED)
  FetchContent_MakeAvailable(libssp)
  add_library(libssp INTERFACE)
  target_include_directories(libssp INTERFACE ${libssp_SOURCE_DIR}/include)
  if(OS_WINDOWS)
    target_link_libraries(
      libssp INTERFACE "${libssp_SOURCE_DIR}/lib/win_x64_vs2017/libssp$<$<CONFIG:Debug>:d>.lib")
    set(LIBSSP_LIBRARY
        "${libssp_SOURCE_DIR}/lib/win_x64_vs2017/libssp$<$<CONFIG:Debug>:d>.dll"
        PARENT_SCOPE)
  elseif(OS_MACOS)
    set(_libssp_x86 "${libssp_SOURCE_DIR}/lib/mac/libssp.dylib")
    set(_libssp_arm "${libssp_SOURCE_DIR}/lib/mac_arm64/libssp.dylib")
    set(_libssp_universal "${CMAKE_CURRENT_BINARY_DIR}/libssp-universal/libssp.dylib")
    set(_build_x86 FALSE)
    set(_build_arm FALSE)
    foreach(_arch IN LISTS CMAKE_OSX_ARCHITECTURES)
      if(_arch STREQUAL "x86_64")
        set(_build_x86 TRUE)
      elseif(_arch STREQUAL "arm64")
        set(_build_arm TRUE)
      endif()
    endforeach()

    if(_build_x86 AND _build_arm)
      find_program(LIPO lipo REQUIRED)
      add_custom_command(
        OUTPUT "${_libssp_universal}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_BINARY_DIR}/libssp-universal"
        COMMAND "${LIPO}" -create "${_libssp_x86}" "${_libssp_arm}"
                -output "${_libssp_universal}"
        DEPENDS "${_libssp_x86}" "${_libssp_arm}"
        VERBATIM)
      add_custom_target(libssp-universal ALL DEPENDS "${_libssp_universal}")
      set(_libssp_selected "${_libssp_universal}")
    elseif(_build_arm)
      set(_libssp_selected "${_libssp_arm}")
    else()
      set(_libssp_selected "${_libssp_x86}")
    endif()
    set(LIBSSP_LIBRARY "${_libssp_selected}" PARENT_SCOPE)
    target_link_libraries(libssp INTERFACE "${_libssp_selected}")
  endif()
endif()

set(SSP_CONNECTOR_SOURCE main.cpp)

add_executable(ssp-connector ${SSP_CONNECTOR_SOURCE})
if(OS_MACOS AND CMAKE_OSX_ARCHITECTURES)
  set_target_properties(
    ssp-connector PROPERTIES OSX_ARCHITECTURES "${CMAKE_OSX_ARCHITECTURES}")
endif()
target_link_libraries(ssp-connector PRIVATE libssp)
if(TARGET libssp-universal)
  add_dependencies(ssp-connector libssp-universal)
endif()
target_include_directories(ssp-connector PRIVATE libuv/include)

# macOS plugin bundles keep libssp.dylib in Contents/Frameworks. The connector
# sits in Contents/MacOS, so give it an @loader_path rpath into Frameworks and
# make libssp load as @rpath/libssp.dylib there instead of the build-tree
# absolute path it is linked against — otherwise a packaged build cannot find
# libssp on another machine.
if(OS_MACOS)
  set_target_properties(
    ssp-connector PROPERTIES
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS
    "${CMAKE_CURRENT_LIST_DIR}/../cmake/bundle/macOS/entitlements.plist"
    BUILD_RPATH "@loader_path/../Frameworks"
    INSTALL_RPATH "@loader_path/../Frameworks")
endif()

if(OS_WINDOWS)
  target_compile_definitions(ssp-connector PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
