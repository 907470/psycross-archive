file(GLOB_RECURSE PSYCROSS_SRCS_C
	"*.c"
)

# The PSYQ surface lives under src/psx/*.C (uppercase) — Windows-derived
# filenames the original devs never normalised. Linux globs are case-
# sensitive, so we list those explicitly to avoid linker misses.
file(GLOB_RECURSE PSYCROSS_SRCS_PSYQ
	"src/psx/*.C"
)

file(GLOB_RECURSE PSYCROSS_SRCS_CPP
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

find_package(SDL2 REQUIRED)
target_link_libraries(psycross_static ${SDL2_LIBRARIES})
target_include_directories(psycross_static PRIVATE ${SDL2_INCLUDE_DIRS})

find_package(OpenAL REQUIRED)
target_link_libraries(psycross_static ${OPENAL_LIBRARY})
target_include_directories(psycross_static PRIVATE ${OPENAL_INCLUDE_DIR})