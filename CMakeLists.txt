# Fastix top-level build. Use tools/build.sh, which sets the toolchain.
cmake_minimum_required(VERSION 3.25)

set(FASTIX_TARGET "aarch64-qemu-virt" CACHE STRING "Board config in config/")
set(SEL4_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/seL4")
set(SEL4_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/seL4_tools")

list(APPEND CMAKE_MODULE_PATH "${SEL4_DIR}" "${SEL4_TOOLS_DIR}/cmake-tool/helpers"
     "${SEL4_TOOLS_DIR}/elfloader-tool")

include(application_settings)
include("${CMAKE_CURRENT_SOURCE_DIR}/config/${FASTIX_TARGET}.cmake")
find_package(seL4 REQUIRED)
find_package(elfloader-tool REQUIRED)
sel4_configure_platform_settings()
ApplyData61ElfLoaderSettings(${KernelPlatform} ${KernelSel4Arch})

project(fastix C ASM)

include("${SEL4_TOOLS_DIR}/cmake-tool/helpers/cpio.cmake")
include("${SEL4_TOOLS_DIR}/cmake-tool/helpers/rootserver.cmake")

sel4_import_kernel()
sel4_import_libsel4()

# The elfloader links "gcc" (libgcc) by name; nothing in it needs libgcc on
# AArch64 with clang, so satisfy the name with an empty interface target.
add_library(gcc INTERFACE)
add_subdirectory(third_party/util_libs/libcpio)
elfloader_import_project()

# seL4 headers are third-party: include them as system headers so the strict
# Fastix warnings below never fire on them (AGENTS.md rule 3).
set_target_properties(sel4 sel4_autoconf PROPERTIES SYSTEM TRUE)

# Strict flags for Fastix code that includes seL4 headers (AGENTS.md rule 3).
# gnu17, not c17: libsel4 headers use the GNU 'asm' keyword.
set(FASTIX_STRICT_FLAGS
    -std=gnu17 -Wall -Wextra -Werror -Wpedantic -Wshadow -Wformat=2 -Wundef
    -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -fno-common -fno-strict-aliasing
    -ffreestanding -fno-pic -fno-stack-protector -O2 -g)

# Read a constant from the single-source headers (AGENTS.md rule 2).
function(fastix_header_value header name out)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/fastix/${header}" line
         REGEX "^#define ${name} ")
    string(REGEX MATCH "0x[0-9A-Fa-f]+|[0-9]+" value "${line}")
    if(value STREQUAL "")
        message(FATAL_ERROR "${name} not found in include/fastix/${header}")
    endif()
    set(${out} "${value}" PARENT_SCOPE)
endfunction()
fastix_header_value(vspace.h FASTIX_EXEC_BASE FASTIX_EXEC_BASE)

# Common settings for Fastix code that runs on seL4.
function(fastix_sel4_target target)
    target_include_directories(${target} PRIVATE include arch/${KernelSel4Arch}/include)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:${FASTIX_STRICT_FLAGS}>)
    target_link_libraries(${target} PRIVATE sel4 sel4_autoconf)
endfunction()

# --- libos: freestanding runtime for servers ---
add_library(libos STATIC
    libs/libos/assert.c
    libs/libos/alloc/slab.c
    libs/libos/alloc/ut_buddy.c
    libs/libos/cap/slot_alloc.c
    libs/libos/ds/rtree.c
    libs/libos/elf/elf.c
    libs/libos/fmt/kprint.c
    libs/libos/mem/mem.c)
fastix_sel4_target(libos)
target_include_directories(libos PUBLIC libs/libos/include)

# --- test programs (embedded in procd until vfsd can load files) ---
set(TEST_LINK_FLAGS -nostdlib -Wl,-e,_start -Wl,-z,max-page-size=4096 -Wl,--build-id=none)
function(fastix_test_program name)
    add_executable(${name} tests/vm/start.S ${ARGN})
    fastix_sel4_target(${name})
    target_include_directories(${name} PRIVATE libs/libfastix_sys/include)
    target_link_libraries(${name} PRIVATE libos)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

fastix_test_program(vmtest tests/vm/vmtest.c)
target_link_options(vmtest PRIVATE ${TEST_LINK_FLAGS} -static -Wl,--image-base=${FASTIX_EXEC_BASE})
fastix_test_program(hello_static tests/vm/hello.c)
target_link_options(hello_static PRIVATE ${TEST_LINK_FLAGS} -static
    -Wl,--image-base=${FASTIX_EXEC_BASE})
fastix_test_program(hello_pie tests/vm/hello.c)
target_compile_definitions(hello_pie PRIVATE HELLO_PIE)
target_compile_options(hello_pie PRIVATE -fPIE)
target_link_options(hello_pie PRIVATE ${TEST_LINK_FLAGS} -Wl,-pie -Wl,--no-dynamic-linker)

# Embed stripped copies of the programs into procd's read-only data
# (order = image index; images.c). Unstripped files stay in build/ for debugging.
set(FASTIX_IMAGES vmtest hello_static hello_pie)
set(IMAGES_ASM "${CMAKE_CURRENT_BINARY_DIR}/images.S")
set(images_text "")
set(image_files "")
foreach(img IN LISTS FASTIX_IMAGES)
    string(APPEND images_text
"    .section .rodata.image_${img}, \"a\"
    .balign 16
    .global fastix_image_${img}_start
fastix_image_${img}_start:
    .incbin \"${CMAKE_CURRENT_BINARY_DIR}/${img}.stripped\"
    .global fastix_image_${img}_end
fastix_image_${img}_end:
")
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${img}.stripped"
        COMMAND ${CMAKE_OBJCOPY} --strip-all "${CMAKE_CURRENT_BINARY_DIR}/${img}"
                "${CMAKE_CURRENT_BINARY_DIR}/${img}.stripped"
        DEPENDS ${img} VERBATIM)
    list(APPEND image_files "${CMAKE_CURRENT_BINARY_DIR}/${img}.stripped")
endforeach()
add_custom_target(stripped_images DEPENDS ${image_files})
file(WRITE "${IMAGES_ASM}" "${images_text}")
set_source_files_properties("${IMAGES_ASM}" PROPERTIES OBJECT_DEPENDS "${image_files}")

# --- procd: the seL4 root task ---
add_executable(procd
    arch/${KernelSel4Arch}/crt0.S
    servers/procd/cspace.c
    servers/procd/frame.c
    servers/procd/heap.c
    servers/procd/images.c
    servers/procd/kobj.c
    servers/procd/loader.c
    servers/procd/main.c
    servers/procd/proc.c
    servers/procd/reply.c
    servers/procd/server.c
    servers/procd/vdso.c
    servers/procd/vmspace.c
    "${IMAGES_ASM}")
fastix_sel4_target(procd)
target_compile_definitions(procd PRIVATE FASTIX_TEST_HOOKS) # test-only procd operations
target_link_libraries(procd PRIVATE libos)
target_link_options(procd PRIVATE -nostdlib -static)
add_dependencies(procd stripped_images)
DeclareRootserver(procd)

# Bootable image: elfloader + kernel + procd in one ELF.
get_property(fastix_image_rel TARGET rootserver_image PROPERTY IMAGE_NAME)
set(FASTIX_IMAGE "${CMAKE_BINARY_DIR}/fastix-image.elf")
add_custom_command(
    OUTPUT "${FASTIX_IMAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/${fastix_image_rel}" "${FASTIX_IMAGE}"
    DEPENDS rootserver_image "${CMAKE_BINARY_DIR}/${fastix_image_rel}"
    VERBATIM)
add_custom_target(fastix_image ALL DEPENDS "${FASTIX_IMAGE}")

# QEMU settings for tests/run_qemu_tests.py, taken from the kernel config so
# the runner always boots the same machine the kernel's device tree describes.
file(WRITE "${CMAKE_BINARY_DIR}/qemu.json"
"{
  \"binary\": \"qemu-system-${QEMU_ARCH}\",
  \"machine\": \"virt,secure=off,gic-version=${QEMU_GIC_VERSION}\",
  \"cpu\": \"${ARM_CPU}\",
  \"memory_mib\": ${QEMU_MEMORY},
  \"image\": \"${FASTIX_IMAGE}\"
}
")
