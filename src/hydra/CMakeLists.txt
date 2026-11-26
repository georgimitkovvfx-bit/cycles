# SPDX-FileCopyrightText: 2022 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

#####################################################################
# Cycles Hydra render delegate
#####################################################################

set(INC
  ..
)
set(INC_SYS
)

set(LIB
  cycles_scene
  cycles_session
  cycles_graph
  PUBLIC bf::dependencies::openimageio
  bf::dependencies::epoxy
)
cycles_external_libraries_append(LIB)
# No Python library in the plugin (see the Python headers below).
list(REMOVE_ITEM LIB bf::dependencies::optional::python)

set(SRC_HD_CYCLES_HEADERS
  camera.h
  config.h
  curves.h
  field.h
  file_reader.h
  geometry.h
  geometry.inl
  implicit_mesh.h
  instancer.h
  bake.h
  cylinder_light.h
  dome_texture.h
  light.h
  link_prefix_scene_index.h
  material.h
  mesh.h
  mesh_attributes.h
  mesh_light.h
  node_util.h
  output_driver.h
  parallax_occlusion.h
  pointcloud.h
  render_buffer.h
  render_delegate.h
  render_pass.h
  render_pass_scene_index.h
  render_settings.h
  resync_scene_index.h
  scene_indices.h
  session.h
  util.h
  volume.h
)

set(SRC_HD_CYCLES
  curves.cpp
  camera.cpp
  field.cpp
  file_reader.cpp
  geometry.cpp
  implicit_mesh.cpp
  instancer.cpp
  bake.cpp
  cylinder_light.cpp
  dome_texture.cpp
  light.cpp
  link_prefix_scene_index.cpp
  material.cpp
  mesh.cpp
  mesh_light.cpp
  node_util.cpp
  output_driver.cpp
  parallax_occlusion.cpp
  pointcloud.cpp
  render_buffer.cpp
  render_delegate.cpp
  render_pass.cpp
  render_pass_scene_index.cpp
  render_settings.cpp
  resync_scene_index.cpp
  scene_indices.cpp
  session.cpp
  util.cpp
  volume.cpp
)

# Blender libraries do not include hgiGL, so build without display driver then.
if(EXISTS ${USD_INCLUDE_DIR}/pxr/imaging/hgiGL)
  add_definitions(-DWITH_HYDRA_DISPLAY_DRIVER)
  list(APPEND SRC_HD_CYCLES display_driver.cpp)
  list(APPEND SRC_HD_CYCLES_HEADERS display_driver.h)
endif()

# MaterialX materials, translated with the MaterialX libraries USD was built with: USD's hdMtlx
# (Houdini, Pixar's USD), or Blender's USD, which is one library, with Blender's MaterialX.
if(TARGET hdMtlx OR WITH_MATERIALX)
  add_definitions(-DWITH_HYDRA_MATERIALX)
  list(APPEND SRC_HD_CYCLES materialx.cpp)
  list(APPEND SRC_HD_CYCLES_HEADERS materialx.h)
  if(TARGET hdMtlx)
    list(APPEND LIB hdMtlx)
  else()
    list(APPEND LIB MaterialXCore MaterialXFormat)
  endif()
endif()

# Volumes from SOPs in a Houdini LOP stage and Houdini image formats (.rat), read through
# Houdini's libraries.
if(HOUDINI_FOUND)
  set(_hdk_sources houdini_image.cpp)
  list(APPEND SRC_HD_CYCLES houdini_image.cpp)
  list(APPEND SRC_HD_CYCLES_HEADERS houdini_image.h)
  add_definitions(-DWITH_HOUDINI_IMAGES)
  if(WITH_OPENVDB)
    list(APPEND _hdk_sources houdini_volume.cpp)
    list(APPEND SRC_HD_CYCLES houdini_volume.cpp)
    list(APPEND SRC_HD_CYCLES_HEADERS houdini_volume.h)
    add_definitions(-DWITH_HOUDINI_VOLUMES)
  endif()
  # Houdini Development Kit definitions, see toolkit/cmake/HoudiniConfig.cmake.
  set(_hdk_definitions
    SIZEOF_VOID_P=8 SESI_LITTLE_ENDIAN UT_ASSERT_LEVEL=0 MAKING_DSO
    FBX_ENABLED=1 OPENCL_ENABLED=1 OPENVDB_ENABLED=1 USE_VULKAN USE_QT6=1
    HBOOST_ALL_NO_LIB HBOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT=1 HBOOST_BIND_GLOBAL_PLACEHOLDERS=1
    EIGEN_MALLOC_ALREADY_ALIGNED=0 _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS=1 _USE_MATH_DEFINES
  )
  if(WIN32)
    list(APPEND _hdk_definitions WIN32 AMD64 I386 SWAP_BITFIELDS STRICT WIN32_LEAN_AND_MEAN)
  elseif(APPLE)
    list(APPEND _hdk_definitions MBSD MBSD_COCOA MBSD_INTEL)
  else()
    # As Houdini's toolkit/cmake/HoudiniConfig.cmake on Linux (UT_Thread needs the pthread ones).
    list(APPEND _hdk_definitions LINUX AMD64 _GNU_SOURCE GCC3 GCC4 ENABLE_THREADS USE_PTHREADS _REENTRANT
         _FILE_OFFSET_BITS=64 _GLIBCXX_PERMIT_BACKWARD_HASH)
  endif()
  set_source_files_properties(${_hdk_sources} PROPERTIES COMPILE_DEFINITIONS "${_hdk_definitions}")
  unset(_hdk_definitions)
  unset(_hdk_sources)
  # Houdini libraries used by the VDB and volume primitives, image files, op: images (TIL, PXL) and
  # their headers.
  # Linux ships them combined: GT, GU and GEO in libHoudiniGEO, UT, CE, IMG, TIL and PXL in libHoudiniUT.
  if(UNIX AND NOT APPLE)
    set(_hdk_libs HoudiniGEO HoudiniUT)
  else()
    set(_hdk_libs GT GU GEO UT CE IMG TIL PXL)
  endif()
  foreach(_hdk_lib ${_hdk_libs})
    find_library(_hdk_library NAMES lib${_hdk_lib} ${_hdk_lib} HOUDINI_${_hdk_lib}
      PATHS ${HOUDINI_ROOT}/custom/houdini/dsolib ${HOUDINI_ROOT}/dsolib
      ${HOUDINI_ROOT}/Frameworks/Houdini.framework/Libraries NO_DEFAULT_PATH)
    list(APPEND LIB ${_hdk_library})
    unset(_hdk_library CACHE)
  endforeach()
endif()

# Silence warning from USD headers using deprecated TBB header.
add_definitions(
  -D__TBB_show_deprecation_message_atomic_H
  -D__TBB_show_deprecation_message_task_H
)

include_directories(${INC})
include_directories(SYSTEM ${INC_SYS})

add_library(cycles_hydra STATIC
  ${SRC_HD_CYCLES}
  ${SRC_HD_CYCLES_HEADERS}
)

target_compile_options(cycles_hydra
  PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/wd4003 /wd4244 /wd4506>
  $<$<CXX_COMPILER_ID:GNU>:-Wno-float-conversion -Wno-double-promotion -Wno-deprecated>
)

target_compile_definitions(cycles_hydra
  PRIVATE
  GLOG_NO_ABBREVIATED_SEVERITIES=1
  OSL_DEBUG=$<CONFIG:DEBUG>
  TBB_USE_DEBUG=$<CONFIG:DEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX=1>
)

target_link_libraries(cycles_hydra
  PUBLIC
  bf::dependencies::openimageio
  bf::dependencies::optional::tbb
  bf::dependencies::optional::usd
  PRIVATE
  ${LIB}
)

# Python's headers only: USD's headers include them, the delegate itself uses no Python. The
# library is the host's (Blender's, which it exports, or the Python USD links): a Python library
# linked into the plugin would be a second, uninitialised interpreter (usdview crashed in
# PyDict_New with Cycles' static Python before USD's on the link line).
if(PYTHON_INCLUDE_DIR)
  target_include_directories(cycles_hydra SYSTEM PUBLIC ${PYTHON_INCLUDE_DIR})
endif()

if(WITH_CYCLES_HYDRA_RENDER_DELEGATE)
  set(SRC_HD_CYCLES_PLUGIN
    plugin.h
    plugin.cpp
  )
  set(HdCyclesPluginName hdCycles)
  add_library(${HdCyclesPluginName} SHARED ${SRC_HD_CYCLES_PLUGIN})

  set_target_properties(${HdCyclesPluginName}
    PROPERTIES PREFIX ""
  )

  target_compile_definitions(${HdCyclesPluginName}
    PRIVATE
    MFB_PACKAGE_NAME=${HdCyclesPluginName}
    MFB_ALT_PACKAGE_NAME=${HdCyclesPluginName}
    GLOG_NO_ABBREVIATED_SEVERITIES=1
    OSL_DEBUG=$<CONFIG:DEBUG>
    TBB_USE_DEBUG=$<CONFIG:DEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX=1>
  )

  target_link_libraries(${HdCyclesPluginName}
    cycles_hydra
  )

  # UsdImaging adapters in their own library that only links USD: UsdImaging loads keyless
  # adapters in every session, so in hdCycles they would load Cycles for any renderer.
  set(HdCyclesImagingName hdCyclesImaging)
  add_library(${HdCyclesImagingName} SHARED imaging_plugin.cpp)
  set_target_properties(${HdCyclesImagingName}
    PROPERTIES PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  )
  target_compile_definitions(${HdCyclesImagingName}
    PRIVATE
    MFB_PACKAGE_NAME=${HdCyclesImagingName}
    MFB_ALT_PACKAGE_NAME=${HdCyclesImagingName}
    $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX=1>
  )
  target_include_directories(${HdCyclesImagingName} SYSTEM PRIVATE ${USD_INCLUDE_DIR})
  target_link_libraries(${HdCyclesImagingName}
    bf::dependencies::optional::tbb
    bf::dependencies::optional::usd
  )
  if(PYTHON_INCLUDE_DIR)
    target_include_directories(${HdCyclesImagingName} SYSTEM PRIVATE ${PYTHON_INCLUDE_DIR})
  endif()

  if(APPLE)
    set_property(
      TARGET
      ${HdCyclesPluginName}
      APPEND_STRING PROPERTY LINK_FLAGS
      " -Wl,-exported_symbols_list,'${CMAKE_CURRENT_SOURCE_DIR}/resources/apple_symbols.map'"
    )
  elseif(UNIX)
    set_property(
      TARGET
      ${HdCyclesPluginName}
      APPEND_STRING PROPERTY LINK_FLAGS
      " -Wl,--version-script='${CMAKE_CURRENT_SOURCE_DIR}/resources/linux_symbols.map'"
    )
  endif()


  if(WITH_CYCLES_BLENDER)
    # Install inside add-on
    set(CYCLES_HYDRA_INSTALL_PATH ${CYCLES_INSTALL_PATH}/hydra)
  elseif(HOUDINI_FOUND)
    set(CYCLES_HYDRA_INSTALL_PATH ${CYCLES_INSTALL_PATH}/houdini/dso)
  else()
    # Install next to cycles executable
    set(CYCLES_HYDRA_INSTALL_PATH ${CMAKE_INSTALL_PREFIX}/hydra)
  endif()

  # The plugins are installed as copies of the built files (delayed_install), so they never get CMake's install
  # rpath and kept the build machine's library folders, where e.g. Embree's libsycl is not next to Embree: they
  # failed to load on Linux. Look next to the installed plugins first (hdcycles-blender patch 0025).
  if(UNIX AND NOT APPLE AND NOT WITH_CYCLES_BLENDER)
    if(HOUDINI_FOUND)
      set(_hydra_lib_rpath "$ORIGIN/../../lib")
    else()
      set(_hydra_lib_rpath "$ORIGIN/../lib")
    endif()
    foreach(_hydra_target ${HdCyclesPluginName} ${HdCyclesImagingName})
      get_target_property(_hydra_build_rpath ${_hydra_target} BUILD_RPATH)
      if(NOT _hydra_build_rpath)
        set(_hydra_build_rpath "")
      endif()
      set_target_properties(${_hydra_target} PROPERTIES BUILD_RPATH "${_hydra_lib_rpath};${_hydra_build_rpath}")
    endforeach()
    unset(_hydra_target)
    unset(_hydra_lib_rpath)
    unset(_hydra_build_rpath)
  endif()

  if(HOUDINI_FOUND)
    delayed_install("${CMAKE_CURRENT_SOURCE_DIR}" "plugInfo.json" ${CYCLES_HYDRA_INSTALL_PATH}/usd_plugins)
    delayed_install("" $<TARGET_FILE:${HdCyclesPluginName}> ${CYCLES_HYDRA_INSTALL_PATH}/usd)
    delayed_install("" $<TARGET_FILE:${HdCyclesImagingName}> ${CYCLES_HYDRA_INSTALL_PATH}/usd)
  else()
    # Put the root `plugInfo.json` one level up.
    delayed_install("${CMAKE_CURRENT_SOURCE_DIR}" "plugInfo.json" ${CYCLES_HYDRA_INSTALL_PATH})
    delayed_install("" $<TARGET_FILE:${HdCyclesPluginName}> ${CYCLES_HYDRA_INSTALL_PATH})
    delayed_install("" $<TARGET_FILE:${HdCyclesImagingName}> ${CYCLES_HYDRA_INSTALL_PATH})
  endif()

  set_target_properties(${HdCyclesPluginName}
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  set(PLUG_INFO_ROOT "..")

  if(WITH_CYCLES_BLENDER)
    # Full path not yet known at this point and RELATIVE_PATH requires
    # absolute path as input. So just set manually.
    set(PLUG_INFO_LIBRARY_PATH "../${HdCyclesPluginName}${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(PLUG_INFO_RESOURCE_PATH "../..")
  else()
    if (HOUDINI_FOUND)
      file(RELATIVE_PATH
        PLUG_INFO_LIBRARY_PATH
        "${CYCLES_HYDRA_INSTALL_PATH}/usd_plugins/${HdCyclesPluginName}"
        "${CYCLES_HYDRA_INSTALL_PATH}/usd/${HdCyclesPluginName}${CMAKE_SHARED_LIBRARY_SUFFIX}")
      # Relative to the plugin root, which is "usd_plugins/hdCycles" for Houdini.
      file(RELATIVE_PATH PLUG_INFO_RESOURCE_PATH
        "${CYCLES_HYDRA_INSTALL_PATH}/usd_plugins/${HdCyclesPluginName}"
        "${CYCLES_INSTALL_PATH}")
    else()
      file(RELATIVE_PATH
        PLUG_INFO_LIBRARY_PATH
        "${CYCLES_HYDRA_INSTALL_PATH}/${HdCyclesPluginName}"
        "${CYCLES_HYDRA_INSTALL_PATH}/${HdCyclesPluginName}${CMAKE_SHARED_LIBRARY_SUFFIX}")
      file(RELATIVE_PATH PLUG_INFO_RESOURCE_PATH
        "${CYCLES_HYDRA_INSTALL_PATH}/${HdCyclesPluginName}"
        "${CYCLES_INSTALL_PATH}")
    endif()
  endif()

  configure_file(resources/plugInfo.json
    ${CMAKE_CURRENT_BINARY_DIR}/resources/plugInfo.json
    @ONLY
  )

  # The imaging plugin folder is a sibling of the delegate's, its library next to hdCycles.
  string(REPLACE "${HdCyclesPluginName}${CMAKE_SHARED_LIBRARY_SUFFIX}"
    "${HdCyclesImagingName}${CMAKE_SHARED_LIBRARY_SUFFIX}"
    PLUG_INFO_IMAGING_LIBRARY_PATH "${PLUG_INFO_LIBRARY_PATH}")
  configure_file(resources/imaging/plugInfo.json
    ${CMAKE_CURRENT_BINARY_DIR}/imaging/resources/plugInfo.json
    @ONLY
  )

  if(HOUDINI_FOUND)
    delayed_install("${CMAKE_CURRENT_BINARY_DIR}/resources" "plugInfo.json" "${CYCLES_HYDRA_INSTALL_PATH}/usd_plugins/${HdCyclesPluginName}/resources")
    delayed_install("${CMAKE_CURRENT_BINARY_DIR}/imaging/resources" "plugInfo.json" "${CYCLES_HYDRA_INSTALL_PATH}/usd_plugins/${HdCyclesImagingName}/resources")
    # Houdini package, renderer registration and shader nodes.
    add_subdirectory(houdini)
  else()
    delayed_install("${CMAKE_CURRENT_BINARY_DIR}/resources" "plugInfo.json" "${CYCLES_HYDRA_INSTALL_PATH}/${HdCyclesPluginName}/resources")
    delayed_install("${CMAKE_CURRENT_BINARY_DIR}/imaging/resources" "plugInfo.json" "${CYCLES_HYDRA_INSTALL_PATH}/${HdCyclesImagingName}/resources")
  endif()

  if(WITH_CYCLES_OSL)
    # OSL camera for Karma's Physical Lens, compiled when first used (like Blender's scripts).
    delayed_install("${CMAKE_CURRENT_SOURCE_DIR}/resources/osl" "hdcycles_physical_lens.osl" ${CYCLES_INSTALL_PATH}/shader)
  endif()

  if(WITH_CYCLES_BLENDER)
    get_filename_component(_addons_core_dir "${CYCLES_INSTALL_PATH}" DIRECTORY)
    delayed_install(
      "${CMAKE_CURRENT_SOURCE_DIR}/addon"
      "__init__.py"
      "${_addons_core_dir}/hydra_cycles"
    )
    unset(_addons_core_dir)
  endif()
endif()
