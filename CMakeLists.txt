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
    -ffreestanding -fno-pic -fno-stack-protector)

# --- procd: the seL4 root task ---
add_executable(procd
    arch/${KernelSel4Arch}/crt0.S
    libs/libos/assert.c
    libs/libos/fmt/kprint.c
    servers/procd/main.c)
target_include_directories(procd PRIVATE
    include
    arch/${KernelSel4Arch}/include
    libs/libos/include)
target_compile_options(procd PRIVATE $<$<COMPILE_LANGUAGE:C>:${FASTIX_STRICT_FLAGS}>)
target_link_libraries(procd PRIVATE sel4 sel4_autoconf)
target_link_options(procd PRIVATE -nostdlib -static)
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
