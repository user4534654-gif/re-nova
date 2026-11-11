# Core runtime + generated translation + platform layer.

# The translated sources are produced by tools/armrecomp.py.  When this tree
# is only a runtime (the builder checks it out separately), point the build at
# the generated directory with -DNOVA_GENERATED_DIR=<abs path>.
set(NOVA_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH
    "Directory containing functions_*.c, table.c and imports_gen.c")

# Where the double-clickable Nova.exe is written.  Defaults to the runtime
# tree so a manual build behaves like before; the builder points it at the
# final output folder.
set(NOVA_OUTPUT_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH
    "Directory for the release Nova.exe")

set(GENERATED_DIR "${NOVA_GENERATED_DIR}")

# The translator emits one functions_NN.c per chunk (and no monolithic
# functions.c).  Keep a fallback for an older generated tree.
file(GLOB GENERATED_CHUNKS CONFIGURE_DEPENDS "${GENERATED_DIR}/functions_*.c")
if(GENERATED_CHUNKS)
  set(GENERATED_FUNCTION_SOURCES ${GENERATED_CHUNKS})
else()
  set(GENERATED_FUNCTION_SOURCES "${GENERATED_DIR}/functions.c")
endif()

add_library(nova_core STATIC
  core/diag.c
  core/guest.c
  core/imports.c
  core/jni.c
  platform/win32.c
  platform/input_map.c
  platform/kbm.c
  platform/faststart.c
  platform/audio.c
  platform/stb_vorbis.c
  platform/movie.c
  gl/gl_globals.c
  gl/gl_state.c
  gl/gl_matrix.c
  gl/gl_texture.c
  gl/gl_light.c
  gl/gl_arrays.c
  gl/gl_draw.c
  gl/gl_query.c
  gl/gl_capture.c
  gl/gl_diag.c
  gl/gl_missing.c
  gl/gl_dispatch.c
  ${GENERATED_FUNCTION_SOURCES}
  "${GENERATED_DIR}/table.c"
  "${GENERATED_DIR}/imports_gen.c"
  "${GENERATED_DIR}/plt_table.c"
)

target_include_directories(nova_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/core"
  "${CMAKE_CURRENT_SOURCE_DIR}/gl"
  "${GENERATED_DIR}"
)

set(NOVA_WARNINGS
  -O1 -g -fno-strict-aliasing -fwrapv -Wall -Wextra
  -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable
  -Wno-unused-label -Wno-unused-value
)

target_compile_options(nova_core PRIVATE ${NOVA_WARNINGS})
target_link_libraries(nova_core PUBLIC
  ws2_32 opengl32 winmm user32 gdi32 m dinput8 dxguid shell32
  mfreadwrite mfplat mfuuid ole32 uuid)

# Console development tool: CRC oracle, matrix tests, boot with logs.
add_executable(nova main.c)
target_link_libraries(nova PRIVATE nova_core)
target_compile_options(nova PRIVATE ${NOVA_WARNINGS})
set_target_properties(nova PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# Normal double-clickable game executable (Windows GUI subsystem, no console).
add_executable(NovaGame WIN32 main.c)
target_link_libraries(NovaGame PRIVATE nova_core)
target_compile_definitions(NovaGame PRIVATE RECOMP_GUI)
target_compile_options(NovaGame PRIVATE ${NOVA_WARNINGS})
set_target_properties(NovaGame PROPERTIES
  OUTPUT_NAME "Nova"
  RUNTIME_OUTPUT_DIRECTORY "${NOVA_OUTPUT_DIR}"
)
