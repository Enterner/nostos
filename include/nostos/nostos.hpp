#pragma once
// nostos — umbrella header for host code.
//
// Not included by nostos/loader.hpp or nostos/abi/plugin.hpp on purpose:
//   * a host that loads plugins adds "nostos/loader.hpp" (and links
//     nostos::loader, the one compiled part of the library);
//   * a *plugin* includes only "nostos/abi/plugin.hpp" plus the shared
//     interface headers, so it stays small and needs no host-side machinery.

#include "nostos/abi/nostos_abi.h"
#include "nostos/abi/host_api.hpp"
#include "nostos/abi/interface.hpp"

#include "nostos/component.hpp"
#include "nostos/context.hpp"
#include "nostos/di.hpp"
#include "nostos/error.hpp"
#include "nostos/event.hpp"
#include "nostos/executor.hpp"
#include "nostos/guard.hpp"
#include "nostos/host_builder.hpp"
#include "nostos/published.hpp"
#include "nostos/registry.hpp"
#include "nostos/scope.hpp"
#include "nostos/svc_id.hpp"
