# Rate-limit FM4's per-frame developer-studio stub warning.
# Modified 2026-10-18. GPL-3.0-only; underlying SDK source remains BSD-3-Clause.
if(NOT WIN32 OR NOT TARGET rexruntime)
  return()
endif()
set(fm4_stub_input "${CMAKE_SOURCE_DIR}/src/kernel/xam/xam_misc.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fm4_stub_input}")
file(READ "${fm4_stub_input}" fm4_stub_source)
string(REPLACE "\r\n" "\n" fm4_stub_source "${fm4_stub_source}")
set(fm4_stub_old "REX_EXPORT_STUB(__imp__XamXStudioRequest);")
string(FIND "${fm4_stub_source}" "${fm4_stub_old}" fm4_stub_at)
if(fm4_stub_at LESS 0)
  message(FATAL_ERROR "Pinned developer-studio stub does not match the logging correction")
endif()
set(fm4_stub_new [=[// The original stub only logs; preserve every guest register and memory byte.
// FM4 calls it every frame. Repeated synchronous log flushes are unnecessary.
extern "C" REX_FUNC(__imp__XamXStudioRequest) {
  (void)ctx;
  (void)base;
  static std::atomic_flag warned = ATOMIC_FLAG_INIT;
  if (!warned.test_and_set(std::memory_order_relaxed))
    REXKRNL_WARN("__imp__XamXStudioRequest STUB (repeated calls omitted)");
}
static rex::ppc::detail::PPCFuncRegistrar _ppc_reg___imp__XamXStudioRequest(
    "__imp__XamXStudioRequest", &__imp__XamXStudioRequest);]=])
string(REPLACE "${fm4_stub_old}" "${fm4_stub_new}" fm4_stub_source "${fm4_stub_source}")
string(REPLACE "#include <rex/hook.h>" "#include <rex/hook.h>\n#include <atomic>" fm4_stub_source "${fm4_stub_source}")
set(fm4_stub_output "${CMAKE_BINARY_DIR}/fm4-stub-logging/xam_misc.cpp")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/fm4-stub-logging")
set(fm4_stub_previous "")
if(EXISTS "${fm4_stub_output}")
  file(READ "${fm4_stub_output}" fm4_stub_previous)
endif()
if(NOT fm4_stub_previous STREQUAL fm4_stub_source)
  file(WRITE "${fm4_stub_output}" "${fm4_stub_source}")
endif()
get_target_property(fm4_stub_sources rexruntime SOURCES)
set(fm4_stub_replaced 0)
set(fm4_stub_result)
foreach(fm4_entry IN LISTS fm4_stub_sources)
  if(fm4_entry STREQUAL "${fm4_stub_input}")
    list(APPEND fm4_stub_result "${fm4_stub_output}")
    math(EXPR fm4_stub_replaced "${fm4_stub_replaced}+1")
  else()
    list(APPEND fm4_stub_result "${fm4_entry}")
  endif()
endforeach()
if(NOT fm4_stub_replaced EQUAL 1)
  message(FATAL_ERROR "Expected one developer-studio source in the pinned runtime")
endif()
set_property(TARGET rexruntime PROPERTY SOURCES "${fm4_stub_result}")
