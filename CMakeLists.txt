file(GLOB_RECURSE PSYCROSS_SRCS_C CONFIGURE_DEPENDS
	"*.c"
)

# The PSYQ surface lives under src/psx/*.C (uppercase) — Windows-derived
# filenames the original devs never normalised. Linux globs are case-
# sensitive, so we list those explicitly to avoid linker misses.
file(GLOB_RECURSE PSYCROSS_SRCS_PSYQ CONFIGURE_DEPENDS
	"src/psx/*.C"
)

file(GLOB_RECURSE PSYCROSS_SRCS_CPP CONFIGURE_DEPENDS
	"*.cpp"
)

set_source_files_properties( ${PSYCROSS_SRCS_C} PROPERTIES LANGUAGE C)
set_source_files_properties( ${PSYCROSS_SRCS_PSYQ} PROPERTIES LANGUAGE CXX)
set_source_files_properties( ${PSYCROSS_SRCS_CPP} PROPERTIES LANGUAGE CXX)
#set_source_files_properties( "src/gte/PsyX_GTE.cpp" PROPERTIES LANGUAGE C)

# rcossin_tbl.h is a generated brace-init-list of 16-bit values that
# triggers narrowing-conversion errors under modern GCC C++17. The
# values fit; the warning is conservative. Mute it for the PSYQ TUs.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	set_source_files_properties(${PSYCROSS_SRCS_PSYQ}
		PROPERTIES COMPILE_OPTIONS "-Wno-narrowing;-Wno-format;-fpermissive")
endif()

add_library(psycross_static STATIC
	${PSYCROSS_SRCS_C}
	${PSYCROSS_SRCS_PSYQ}
	${PSYCROSS_SRCS_CPP}
)
set_target_properties(psycross_static PROPERTIES OUTPUT_NAME "psycross")
target_include_directories(psycross_static PUBLIC "include" "include/psx")

# PGXP enabled. VERTTYPE is now `short` unconditionally (see
# include/PsyX/common/pgxp_defs.h) so the OpenPete bridge's verbatim
# memcpy of PSX P_TAG payloads doesn't collide with PsyCross's vertex
# slot layout. Sub-pixel precision is carried in the parallel float
# px/py/pz fields of PGXPVData; the cache hash is keyed on the int16
# SXY pattern that matches between the GTE emit side and the GPU
# consumer side. Cache population for gameplay GTE ops is wired in
# pcsx_redux_compat.h's PGXP_pushSXYZ2s adapter.
target_compile_definitions(psycross_static PUBLIC USE_PGXP=1)

# SDL3 (built from openpete's third_party/SDL submodule). The SDL3::SDL3
# target carries its own include dirs transitively.
target_link_libraries(psycross_static SDL3::SDL3-static)

find_package(OpenAL REQUIRED)
target_link_libraries(psycross_static ${OPENAL_LIBRARY})
target_include_directories(psycross_static PRIVATE ${OPENAL_INCLUDE_DIR})

# Emscripten compiles with clang, which (unlike the GCC native build)
# treats braced-init narrowing as a hard error rather than a warning —
# e.g. the generated 16-bit tables in gte/half_float.cpp + rcossin_tbl.h.
# The values fit; demote it back to a warning for the whole vendored lib.
if(EMSCRIPTEN)
    target_compile_options(psycross_static PRIVATE -Wno-c++11-narrowing)
endif()