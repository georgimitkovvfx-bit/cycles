# SPDX-FileCopyrightText: 2011-2022 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

#####################################################################
# Cycles standalone executable
#####################################################################

set(INC
  ..
)
set(INC_SYS
)

set(LIB
  cycles_device
  cycles_kernel
  cycles_scene
  cycles_session
  cycles_bvh
  cycles_subd
  cycles_graph
  cycles_util
)

if(WITH_CYCLES_OSL)
  list(APPEND LIB cycles_kernel_osl)
endif()

if(CYCLES_STANDALONE_REPOSITORY)
  list(APPEND LIB extern_sky)
else()
  list(APPEND LIB bf_intern_sky)
endif()

if(WITH_CYCLES_STANDALONE AND WITH_CYCLES_STANDALONE_GUI)
  list(APPEND LIB
    bf::dependencies::epoxy
    bf::dependencies::optional::sdl
  )
endif()

if(WITH_USD)
  # Silence warning from USD headers using deprecated TBB header.
  add_definitions(
    -D__TBB_show_deprecation_message_atomic_H
    -D__TBB_show_deprecation_message_task_H
  )
  list(APPEND LIB
    cycles_hydra
    bf::dependencies::optional::usd
  )
endif()

cycles_external_libraries_append(LIB)

# Common configuration.

include_directories(${INC})
include_directories(SYSTEM ${INC_SYS})

if(APPLE AND NOT CYCLES_STANDALONE_REPOSITORY)
  set(app_output_dir Blender.app/Contents/MacOS)
  set(app_install_dir Blender.app/Contents/MacOS)
else()
  set(app_install_dir ${CMAKE_INSTALL_PREFIX})
endif()

# Application build targets

if(WITH_CYCLES_STANDALONE)
  set(SRC
    cycles_standalone.cpp
    cycles_xml.cpp
    cycles_xml.h
    oiio_output_driver.cpp
    oiio_output_driver.h
  )

  if(WITH_CYCLES_STANDALONE_GUI)
    list(APPEND SRC
      opengl/display_driver.cpp
      opengl/display_driver.h
      opengl/shader.cpp
      opengl/shader.h
      opengl/window.cpp
      opengl/window.h
    )
  endif()

  add_executable(cycles ${SRC} ${INC} ${INC_SYS})
  unset(SRC)

  target_link_libraries(cycles PRIVATE ${LIB})

  if(APPLE)
    if(WITH_CYCLES_STANDALONE_GUI)
      # Frameworks used by SDL.
      string(CONCAT _cycles_sdl_frameworks
        " -framework AudioToolbox"
        " -framework AudioUnit"
        " -framework Cocoa"
        " -framework CoreAudio"
        " -framework CoreHaptics"
        " -framework CoreVideo"
        " -framework ForceFeedback"
        " -framework GameController"
      )
      set_property(
        TARGET cycles
        APPEND PROPERTY LINK_FLAGS
        "${_cycles_sdl_frameworks}"
      )
    endif()
  endif()

  if(CYCLES_STANDALONE_REPOSITORY)
    cycles_install_libraries(cycles)

    if(WITH_USD AND USD_LIBRARY_DIR)
      install(DIRECTORY
        ${USD_LIBRARY_DIR}/usd
        DESTINATION ${CMAKE_INSTALL_PREFIX}
      )

      install(DIRECTORY
        ${USD_LIBRARY_DIR}/../plugin/usd
        DESTINATION ${CMAKE_INSTALL_PREFIX}
      )
    endif()
  endif()

  install(
    TARGETS cycles
    DESTINATION ${app_install_dir})

  if(DEFINED app_output_dir)
    set_target_properties(cycles
      PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${app_output_dir}")
  endif()

  add_test(
    NAME cycles_version
    COMMAND ${app_install_dir}/$<TARGET_FILE_NAME:cycles> --version)
endif()

if(WITH_CYCLES_PRECOMPUTE)
  set(SRC
    cycles_precompute.cpp
  )

  add_executable(cycles_precompute ${SRC} ${INC} ${INC_SYS})
  unset(SRC)

  target_link_libraries(cycles_precompute
    PRIVATE cycles_util
    PRIVATE bf::dependencies::openimageio
    PRIVATE bf::dependencies::optional::tbb
    PRIVATE bf::dependencies::optional::osl)

  if(NOT CYCLES_STANDALONE_REPOSITORY)
    target_link_libraries(cycles_precompute
      PRIVATE bf::intern::guardedalloc)
  endif()

  if(DEFINED app_output_dir)
    set_target_properties(cycles_precompute
      PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${app_output_dir}")
  endif()

  install(
    TARGETS cycles_precompute
    DESTINATION ${app_install_dir})
endif()

# USD Nodes (hdcycles-blender patch 0010): frames of a USD file in one Cycles session, without Blender.
if(WITH_CYCLES_HYDRA_RENDER_DELEGATE AND WITH_USD)
  add_executable(cycles_usd_render
    cycles_usd_render.cpp
  )
  target_link_libraries(cycles_usd_render PRIVATE ${LIB})
  # Copied as built, keeping the build RUNPATH to Blender's prebuilt libraries (install(TARGETS) would reset it).
  install(PROGRAMS $<TARGET_FILE:cycles_usd_render> DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
  if(USD_LIBRARY)
    # Blender's libusd_ms needs MaterialX; inside Blender it is already loaded, an executable links it itself.
    get_filename_component(_usd_lib_dir ${USD_LIBRARY} DIRECTORY)
    file(GLOB _materialx_libs ${_usd_lib_dir}/../../materialx/lib/libMaterialX*.so)
    target_link_libraries(cycles_usd_render PRIVATE ${_materialx_libs})
    get_filename_component(_materialx_dir ${_usd_lib_dir}/../../materialx/lib ABSOLUTE)
    # DT_RPATH (not RUNPATH): libusd_ms and the other prebuilt libraries find their own dependencies through it too.
    # --exclude-libs: cuew's symbols stay private. Exported (Python's static library brings -export-dynamic, which
    # libusd_ms needs), cuew's `cuInit` pointer shadowed the CUDA driver's for OpenImageDenoise's CUDA module: crash.
    # $ORIGIN/../lib first: a bundle (build.sh bundle) carries its libraries there; else the build's own.
    target_link_options(cycles_usd_render PRIVATE
      "-Wl,--disable-new-dtags" "LINKER:-rpath,$ORIGIN/../lib" "-Wl,-rpath,${_materialx_dir}"
      "-Wl,--exclude-libs,libextern_cuew.a")
    unset(_materialx_dir)
    unset(_materialx_libs)
    # USD's plugins (plugInfo.json; usdShaders etc.): the executable registers <prefix>/usd and <prefix>/usd_plugins.
    get_filename_component(_usd_lib_dir ${USD_LIBRARY} DIRECTORY)
    install(DIRECTORY ${_usd_lib_dir}/usd DESTINATION ${CMAKE_INSTALL_PREFIX})
    install(DIRECTORY ${_usd_lib_dir}/../plugin/usd/ DESTINATION ${CMAKE_INSTALL_PREFIX}/usd_plugins)
    unset(_usd_lib_dir)
  endif()
endif()
