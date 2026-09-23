cmake_minimum_required(VERSION 3.16...3.26)

# Set policy for escape sequences in strings
cmake_policy(SET CMP0010 NEW)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)

set(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
set(ENABLE_QT "Use Qt functionality" ON)

# Zero-copy hardware decode + camera-control dock
set(ENABLE_ZERO_COPY "Build zero-copy hardware decode backends" ON)
set(ENABLE_CAMERA_CONTROL "Build the embedded camera-control dock" ON)
# FFmpeg is NOT required for hardware decode (native NVDEC/D3D11VA/VideoToolbox).
# It is used only as a software-decode fallback when no hardware path is
# available.
set(ENABLE_SW_DECODE "Software decode fallback via FFmpeg" ON)
# The dock's request-log panel and its "Debug" button are a development aid;
# they are left out of release builds unless this is switched on.
option(ZC_ENABLE_DEBUG_PANEL "Show the control dock's debug log panel" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  # NOTE: uses Qt::Network, NOT Qt::WebSockets — the Qt6 bundled with OBS
  # ships Network but not WebSockets, so the notification socket is a minimal
  # RFC 6455 client built on QTcpSocket (src/cameractrl/zc_ws_client.*).
  find_package(Qt6 COMPONENTS Widgets Core Network)
  target_link_libraries(${CMAKE_PROJECT_NAME}
                        PRIVATE Qt::Core Qt::Widgets Qt::Network)
  target_compile_options(
    ${CMAKE_PROJECT_NAME} PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header
                                  -Wno-comma>)
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON
               AUTOUIC ON
               AUTORCC ON)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/Modules")

# --- Clean-rewrite source list ---
# hw_decode.c is the backend-agnostic factory: it has no FFmpeg dependency of
# its own and is always compiled, so ENABLE_ZERO_COPY=OFF / ENABLE_SW_DECODE=OFF
# builds still link.
set(obs-zcamera_SOURCES
    src/obs-ssp.cpp
    src/zc_source.cpp
    src/ssp-mdns.cpp
    src/VFrameQueue.cpp
    src/ssp-client-iso.cpp
    src/hwdecode/hw_decode.c)

set(obs-zcamera_HEADERS src/obs-ssp.h src/ssp-mdns.h src/VFrameQueue.h
                        src/ssp-client-iso.h src/hwdecode/hw_decode.h)

if(ENABLE_ZERO_COPY)
  # FFmpeg-hwaccel engine: decodes on any GPU the OS recognizes (CUDA/D3D11VA
  # on Windows, VideoToolbox on macOS) and imports the GPU surface into OBS
  # with no av_hwframe_transfer_data (zero-copy). FFmpeg is required for this.
  list(APPEND obs-zcamera_SOURCES src/hwdecode/hw_decode_ffmpeg.cpp)
  list(APPEND obs-zcamera_HEADERS src/hwdecode/hw_decode_ffmpeg.h)
  find_package(FFmpeg REQUIRED COMPONENTS AVCODEC AVUTIL)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE FFmpeg::avcodec FFmpeg::avutil)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_ZERO_COPY)
  if(WIN32)
    list(APPEND obs-zcamera_SOURCES src/hwdecode/hw_decode_d3d11_win.cpp
         src/hwdecode/hw_decode_cuda_win.cpp)
    # d3dcompiler: the CUDA importer compiles its NV12 -> BGRA pixel-shader pass
    # at runtime. nvcuda.dll is loaded dynamically, so no CUDA toolkit is needed.
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE d3d11 dxgi d3dcompiler)
  elseif(APPLE)
    find_library(VIDEOTOOLBOX_FW VideoToolbox REQUIRED)
    find_library(COREVIDEO_FW CoreVideo REQUIRED)
    find_library(IOSURFACE_FW IOSurface REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME}
                          PRIVATE ${VIDEOTOOLBOX_FW} ${COREVIDEO_FW}
                                  ${IOSURFACE_FW})
  endif()
endif()

# Software decode fallback (FFmpeg). Not required for the zero-copy path.
if(ENABLE_SW_DECODE)
  list(APPEND obs-zcamera_SOURCES src/ffmpeg-decode.c
       src/hwdecode/hw_decode_sw.cpp)
  list(APPEND obs-zcamera_HEADERS src/ffmpeg-decode.h src/hwdecode/hw_decode_sw.h)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_SW_DECODE)
  find_package(FFmpeg REQUIRED COMPONENTS AVCODEC AVUTIL SWSCALE)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale)
endif()

if(ENABLE_CAMERA_CONTROL AND ENABLE_QT AND ENABLE_FRONTEND_API)
  list(APPEND obs-zcamera_SOURCES
       src/cameractrl/zc_http_transport.cpp
       src/cameractrl/zc_ws_client.cpp
       src/cameractrl/zc_ws_notifier.cpp
       src/cameractrl/zc_settings_engine.cpp
       src/cameractrl/zc_camera_client.cpp
       src/cameractrl/zc_settings_schema.cpp
       src/ui/zc_control_dock.cpp
       src/ui/zc_debug_panel.cpp)
  list(APPEND obs-zcamera_HEADERS
       src/cameractrl/zc_http_transport.h
       src/cameractrl/zc_ws_client.h
       src/cameractrl/zc_ws_notifier.h
       src/cameractrl/zc_settings_engine.h
       src/cameractrl/zc_camera_client.h
       src/cameractrl/zc_settings_schema.h
       src/ui/zc_control_dock.h
       src/ui/zc_debug_panel.h)
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${obs-zcamera_SOURCES})

# /!\ TAKE NOTE: No need to edit things past this point /!\

# --- Platform-independent build settings ---
add_subdirectory(thirdpty)

target_include_directories(
  ${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src
                                ${CMAKE_SOURCE_DIR}/src/hwdecode
                                ${CMAKE_SOURCE_DIR}/src/cameractrl
                                ${CMAKE_SOURCE_DIR}/src/ui
                                ${CMAKE_SOURCE_DIR}/lib/ssp/include
                                ${CMAKE_SOURCE_DIR}/ssp_connector)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_HEVC)
if(ENABLE_ZERO_COPY)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_ZERO_COPY)
endif()
if(ENABLE_CAMERA_CONTROL)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_CAMERA_CONTROL)
endif()
if(ZC_ENABLE_DEBUG_PANEL)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ZC_ENABLE_DEBUG_PANEL)
endif()
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE mdns)

# Link with the plugin-support library for PLUGIN_NAME and PLUGIN_VERSION
if(TARGET plugin-support)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE plugin-support)
endif()

add_subdirectory(ssp_connector)

# Installation rules for ssp-connector
if(OS_MACOS)
  install(TARGETS ssp-connector
    DESTINATION "./${CMAKE_PROJECT_NAME}.plugin/Contents/MacOS"
    COMPONENT plugin)
  install(FILES ${LIBSSP_LIBRARY}
    DESTINATION "./${CMAKE_PROJECT_NAME}.plugin/Contents/Frameworks"
    COMPONENT plugin)
elseif(OS_WINDOWS)
  install(TARGETS ssp-connector
    DESTINATION "obs-plugins/64bit"
    COMPONENT plugin)
  install(FILES ${LIBSSP_LIBRARY}
    DESTINATION "obs-plugins/64bit"
    COMPONENT plugin)
else()
  install(TARGETS ssp-connector
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
    COMPONENT plugin)
endif()

# Platform-specific installation rules for the main target
if(OS_MACOS)
  install(TARGETS ${CMAKE_PROJECT_NAME}
    LIBRARY DESTINATION "./${CMAKE_PROJECT_NAME}.plugin/Contents/MacOS"
    COMPONENT plugin)
elseif(OS_WINDOWS)
  install(TARGETS ${CMAKE_PROJECT_NAME}
    RUNTIME DESTINATION "obs-plugins/64bit"
    LIBRARY DESTINATION "obs-plugins/64bit"
    COMPONENT plugin)
else()
  install(TARGETS ${CMAKE_PROJECT_NAME}
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins"
    COMPONENT plugin)
endif()

# Override default set_target_properties_plugin to avoid installing to the wrong location
function(set_target_properties_plugin target)
  set(options "")
  set(oneValueArgs "")
  set(multiValueArgs PROPERTIES)
  cmake_parse_arguments(PARSE_ARGV 0 _STPO "${options}" "${oneValueArgs}" "${multiValueArgs}")

  message(DEBUG "Setting custom properties for target ${target}...")

  while(_STPO_PROPERTIES)
    list(POP_FRONT _STPO_PROPERTIES key value)
    set_property(TARGET ${target} PROPERTY ${key} "${value}")
  endwhile()

  set_target_properties(${target} PROPERTIES VERSION 0 SOVERSION ${PLUGIN_VERSION})

  # Skip the default install behavior since we already have our own install commands
endfunction()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# Add specific install instructions for the plugin itself
if(OS_WINDOWS)
  if(EXISTS "${CMAKE_SOURCE_DIR}/data")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/data/" DESTINATION "data/obs-plugins/${_name}" COMPONENT plugin)
  endif()
elseif(OS_MACOS)
  if(EXISTS "${CMAKE_SOURCE_DIR}/data")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/data/" DESTINATION "./${CMAKE_PROJECT_NAME}.plugin/Contents/Resources" COMPONENT plugin)
  endif()
else()
  if(EXISTS "${CMAKE_SOURCE_DIR}/data")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/data/" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${_name}" COMPONENT plugin)
  endif()
endif()

# CPack configuration
set(CPACK_PACKAGE_NAME "${_name}")
set(CPACK_PACKAGE_VENDOR "")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ZCAM camera source with zero-copy hardware decode for OBS Studio")
set(CPACK_PACKAGE_VERSION ${CMAKE_PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "")
set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_BINARY_DIR}/package")
set(CPACK_PACKAGING_INSTALL_PREFIX "")
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS "")
set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
set(CMAKE_INSTALL_OPENMP_LIBRARIES FALSE)
set(CMAKE_INSTALL_MFC_LIBRARIES FALSE)
set(CPACK_INSTALL_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/cmake_install_post.cmake")
macro(InstallRequiredSystemLibraries)
  # Empty override to prevent any runtime libraries installation
endmacro()

add_custom_target(clean_package
  COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_install_post.cmake"
  COMMENT "Cleaning up package..."
)
set_target_properties(clean_package PROPERTIES FOLDER "Packaging")
if(TARGET package)
  add_dependencies(package clean_package)
endif()

set(CPACK_COMPONENTS_ALL plugin)
set(CPACK_COMPONENT_PLUGIN_DISPLAY_NAME "${_name} Plugin for OBS Studio")
set(CPACK_COMPONENT_PLUGIN_DESCRIPTION "Enables ZCAM camera support with hardware-accelerated decoding in OBS Studio")
set(CPACK_COMPONENT_PLUGIN_REQUIRED TRUE)

if(WIN32)
  install(CODE [[
    file(GLOB_RECURSE runtime_dlls "${CMAKE_INSTALL_PREFIX}/bin/*.dll")
    foreach(dll ${runtime_dlls})
      if(NOT EXISTS "${CMAKE_INSTALL_PREFIX}/bin/64bit")
        file(MAKE_DIRECTORY "${CMAKE_INSTALL_PREFIX}/bin/64bit")
      endif()
      message(STATUS "Moving runtime DLL to bin/64bit: ${dll}")
      get_filename_component(dll_name ${dll} NAME)
      file(RENAME "${dll}" "${CMAKE_INSTALL_PREFIX}/bin/64bit/${dll_name}")
    endforeach()
  ]] COMPONENT plugin)
endif()

if(WIN32)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/64bit")
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/64bit")
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin/64bit")
  if(DEFINED CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "bin/64bit")
  endif()
endif()

if(WIN32)
  find_program(NSIS_PATH makensis)
  if(NSIS_PATH)
    set(CPACK_GENERATOR "ZIP;NSIS")
  else()
    set(CPACK_GENERATOR "ZIP")
  endif()
  set(CPACK_NSIS_PACKAGE_NAME "${_name}")
  set(CPACK_NSIS_DISPLAY_NAME "${_name}")
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
  set(CPACK_NSIS_MODIFY_PATH OFF)
  set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64\\obs-studio")
  set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "${_name}")
  set(CPACK_NSIS_INSTALL_PREFIX "${CPACK_NSIS_INSTALL_ROOT}")
  set(CPACK_NSIS_MUI_FINISHPAGE_RUN "obs64.exe")
  set(CPACK_NSIS_INSTALLED_ICON_NAME "obs64.exe")
elseif(APPLE)
  set(CPACK_GENERATOR "DragNDrop")
  set(CPACK_DMG_VOLUME_NAME "${_name}")
  set(CPACK_DMG_FORMAT "UDZO")
else()
  set(CPACK_GENERATOR "DEB;RPM;TGZ")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "IMVT")
  set(CPACK_DEBIAN_PACKAGE_SECTION "video")
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "obs-studio")
  set(CPACK_RPM_PACKAGE_REQUIRES "obs-studio")
endif()

include(CPack)

if(APPLE)
  install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/locale"
    DESTINATION "${CMAKE_PROJECT_NAME}.plugin/Contents/Resources/data"
    USE_SOURCE_PERMISSIONS
  )
endif()