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
    libs/libos/alloc/ut_buddy.c
    libs/libos/cap/slot_alloc.c
    libs/libos/elf/elf.c
    libs/libos/fmt/kprint.c
    libs/libos/mem/mem.c)
fastix_sel4_target(libos)
target_include_directories(libos PUBLIC libs/libos/include)

# --- test child: unprivileged process used by procd's self-test ---
add_executable(test_child tests/kernel_ipc/child.c)
fastix_sel4_target(test_child)
target_link_libraries(test_child PRIVATE libos)
target_link_options(test_child PRIVATE -nostdlib -static -Wl,-e,child_start
    -Wl,--image-base=${FASTIX_EXEC_BASE} -Wl,-z,max-page-size=4096 -Wl,--build-id=none)

# Embed the child ELF into procd's read-only data.
set(TEST_CHILD_ELF "${CMAKE_CURRENT_BINARY_DIR}/test_child")
set_target_properties(test_child PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set(CHILD_BLOB "${CMAKE_CURRENT_BINARY_DIR}/child_blob.S")
file(WRITE "${CHILD_BLOB}"
"    .section .rodata.test_child, \"a\"
    .balign 16
    .global fastix_test_child_start
fastix_test_child_start:
    .incbin \"${TEST_CHILD_ELF}\"
    .global fastix_test_child_end
fastix_test_child_end:
")
set_source_files_properties("${CHILD_BLOB}" PROPERTIES OBJECT_DEPENDS "${TEST_CHILD_ELF}")

# --- procd: the seL4 root task ---
add_executable(procd
    arch/${KernelSel4Arch}/crt0.S
    servers/procd/kobj.c
    servers/procd/main.c
    servers/procd/proc.c
    servers/procd/selftest.c
    servers/procd/vm.c
    "${CHILD_BLOB}")
fastix_sel4_target(procd)
target_include_directories(procd PRIVATE tests/kernel_ipc)
target_link_libraries(procd PRIVATE libos)
target_link_options(procd PRIVATE -nostdlib -static)
add_dependencies(procd test_child)
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
