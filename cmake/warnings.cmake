# SPDX-License-Identifier: Apache-2.0
#
# The warning set required by CLAUDE.md, plus the optional sanitizers selected
# by the host-asan preset through HAC_SANITIZERS.

function(hac_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Werror -Wconversion -Wshadow
            # C-only diagnostics: guarded so that the C++ simulator GUI added in
            # phase 1 does not choke on options that do not apply to it.
            $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
            $<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>
        )

        if(HAC_SANITIZERS)
            target_compile_options(${target} PRIVATE
                -fsanitize=${HAC_SANITIZERS} -fno-omit-frame-pointer -g)
            target_link_options(${target} PRIVATE -fsanitize=${HAC_SANITIZERS})
        endif()
    endif()
endfunction()
