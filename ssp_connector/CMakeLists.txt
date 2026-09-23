project(ssp-connector)

include(FetchContent)
FetchContent_Declare(
  libssp
  GIT_REPOSITORY "https://github.com/imaginevision/libssp.git"
  GIT_TAG ee754be1d2ed54633ff467a7cd6fec14c9b0b537)

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
    target_link_libraries(libssp INTERFACE ${libssp_SOURCE_DIR}/lib/mac/libssp.dylib)
    set(LIBSSP_LIBRARY
        "${libssp_SOURCE_DIR}/lib/mac/libssp.dylib"
        PARENT_SCOPE)
    set(_CODE_SIGN_IDENTITY "${CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY}")
    if(_CODE_SIGN_IDENTITY)
      set(_COMMAND
          "/usr/bin/codesign --force \
          --sign \"${_CODE_SIGN_IDENTITY}\" \
          --options runtime \
          --entitlements \"${CMAKE_CURRENT_LIST_DIR}/../cmake/bundle/macOS/entitlements.plist\" \
          \"${libssp_SOURCE_DIR}/lib/mac/libssp.dylib\"")
      execute_process(COMMAND /bin/sh -c "${_COMMAND}")
    endif()
  endif()
endif()

set(SSP_CONNECTOR_SOURCE main.cpp)

add_executable(ssp-connector ${SSP_CONNECTOR_SOURCE})
set_target_properties(ssp-connector PROPERTIES OSX_ARCHITECTURES x86_64)
target_link_libraries(ssp-connector PRIVATE libssp)
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
