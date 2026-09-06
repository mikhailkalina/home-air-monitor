# SPDX-License-Identifier: Apache-2.0
#
# The same warning set cmake/warnings.cmake applies on the host
# (-Wall -Wextra -Werror -Wconversion -Wshadow), factored out separately
# because the ESP-IDF build never sees MSVC or HAC_SANITIZERS and including
# the host file would pull in project() / target assumptions that do not
# hold inside an idf_component_register() component.
#
# Applied to hac_core, hac_platform, hac_contract and main -- every component
# under apps/firmware_esp32 that compiles first-party sources. If an IDF
# header itself is ever the source of an unavoidable -Wconversion diagnostic,
# drop the flag for that one component with a comment naming the header,
# rather than loosening it here for all four.

function(hac_apply_warnings_idf target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Werror -Wconversion -Wshadow
    )
endfunction()
