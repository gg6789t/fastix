# Host unit tests for portable libos code, built with the host compiler under
# AddressSanitizer + UndefinedBehaviorSanitizer. Run with tools/test_host.sh.
cmake_minimum_required(VERSION 3.25)
project(fastix_host_tests C)
enable_testing()

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(HOST_FLAGS
    -std=c17 -Wall -Wextra -Werror -Wpedantic -Wshadow -Wformat=2 -Wundef
    -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -fno-common -fno-strict-aliasing
    -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all)

function(host_test name)
    add_executable(${name} ${name}.c ${ARGN})
    target_include_directories(${name} PRIVATE "${ROOT}/include" "${ROOT}/libs/libos/include")
    target_compile_definitions(${name} PRIVATE LIBOS_HOSTED)
    target_compile_options(${name} PRIVATE ${HOST_FLAGS})
    target_link_options(${name} PRIVATE -fsanitize=address,undefined)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

host_test(test_slot_alloc "${ROOT}/libs/libos/cap/slot_alloc.c")
host_test(test_ut_buddy "${ROOT}/libs/libos/cap/slot_alloc.c" "${ROOT}/libs/libos/alloc/ut_buddy.c")
host_test(test_elf "${ROOT}/libs/libos/elf/elf.c")
host_test(test_slab "${ROOT}/libs/libos/alloc/slab.c")
host_test(test_rtree "${ROOT}/libs/libos/ds/rtree.c")
host_test(test_hashtab "${ROOT}/libs/libos/ds/hashtab.c")
host_test(test_radix "${ROOT}/libs/libos/ds/radix.c" "${ROOT}/libs/libos/alloc/slab.c")
host_test(test_vdso)
host_test(test_ring "${ROOT}/libs/libos/ring/ring.c")
host_test(test_namei "${ROOT}/servers/vfsd/namei.c" "${ROOT}/servers/vfsd/vnode.c"
          "${ROOT}/servers/vfsd/tmpfs.c" "${ROOT}/libs/libos/alloc/slab.c"
          "${ROOT}/libs/libos/ds/radix.c")
host_test(test_dtb "${ROOT}/libs/libdriver/dtb.c")
target_include_directories(test_dtb PRIVATE "${ROOT}/libs/libdriver/include")
target_compile_definitions(test_dtb PRIVATE
    FASTIX_QEMU_DTB_PATH="${ROOT}/build/qemu-arm-virt.dtb")
