# SPDX-License-Identifier: Apache-2.0
#
# THE source list for the shared contract bodies in tests/contract/. It is
# included by both the host build (tests/contract/CMakeLists.txt) and the
# ESP-IDF build (apps/firmware_esp32/components/hac_contract/CMakeLists.txt),
# exactly the way cmake/sources_core.cmake feeds both builds of core/ -- so
# that a contract body compiled on the host and the one run at device boot
# are never two files that happen to agree today.
#
# Never copy these paths into a component CMakeLists.txt.
#
# The includer must define HAC_ROOT as the absolute path of the repository root.

if(NOT DEFINED HAC_ROOT)
    message(FATAL_ERROR "HAC_ROOT must be set before including sources_contract.cmake")
endif()

set(HAC_CONTRACT_SOURCES
    ${HAC_ROOT}/tests/contract/port_clock_contract.c
    ${HAC_ROOT}/tests/contract/port_log_contract.c
)

# Resolves "port_clock.h" from ports/, "contract_report.h" and
# "port_clock_contract.h" from this directory.
set(HAC_CONTRACT_INCLUDES
    ${HAC_ROOT}/ports
    ${HAC_ROOT}/tests/contract
)
