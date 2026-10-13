# SPDX-License-Identifier: Apache-2.0
#
# The slideshow test app on a PC: composition only, like apps/simulator.
# The pictures are embedded at build time (../slideshow_images.cmake), which
# takes a Python 3 interpreter -- standard library only. Without one the app
# is left out of the build rather than failing it, as the simulator's window
# is left out without SDL2.

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(WARNING "Python 3 not found: the slideshow test app is not built.")
    return()
endif()

include(${HAC_ROOT}/apps/slideshow/slideshow_images.cmake)

add_executable(slideshow
    main.c
    ${HAC_ROOT}/apps/slideshow/slideshow.c
)
target_include_directories(slideshow PRIVATE ${HAC_ROOT}/apps/slideshow)
target_link_libraries(slideshow PRIVATE hac_core hac_platform_host)

if(TARGET hac_platform_host_gui)
    target_link_libraries(slideshow PRIVATE hac_platform_host_gui)
endif()

slideshow_add_images(slideshow ${Python3_EXECUTABLE})
hac_apply_warnings(slideshow)
