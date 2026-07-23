/*
   This file is part of darktable,
   Copyright (C) 2026 darktable developers.

   darktable is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   darktable is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "lua/develop.h"
#include "common/darktable.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "lua/call.h"
#include "lua/configuration.h"
#include "lua/types.h"
#include <glib.h>

// API contract version of the darktable.develop bindings. Bump whenever the
// wire shapes (see PLAN.md §3.1) change so the bridge can refuse a mismatch.
#define DT_LUA_DEVELOP_API 1
#define DT_LUA_DEVELOP_MIN_BRIDGE 1

/* darktable.develop.version()
   Pure constant handshake (PLAN.md §3.5). Touches no develop/pipe state, so it
   is NOT wrapped in dt_lua_gtk_wrap. Called with no arguments; returns one
   table on the stack. */
static int version_cb(lua_State *L)
{
  lua_newtable(L);
  lua_pushinteger(L, DT_LUA_DEVELOP_API);
  lua_setfield(L, -2, "api");
  lua_pushfstring(L, "%d.%d.%d", LUA_API_VERSION_MAJOR, LUA_API_VERSION_MINOR, LUA_API_VERSION_PATCH);
  lua_setfield(L, -2, "dt_lua_api");
  lua_pushinteger(L, DT_LUA_DEVELOP_MIN_BRIDGE);
  lua_setfield(L, -2, "min_bridge");
  return 1;
}

/* darktable.develop.active_modules()
   Walk darktable->develop->iop and return an array of module descriptors for
   the image currently open in darkroom. Reads live develop/pipe state, so it
   MUST run on the GUI thread -- registered wrapped in dt_lua_gtk_wrap.
   If no darkroom image is loaded (darktable->develop NULL, or its iop list is
   empty), returns an empty array and logs a clear note rather than crashing. */
static int active_modules_cb(lua_State *L)
{
  dt_develop_t *dev = darktable.develop;

  if(!dev || !dev->iop)
  {
    dt_print(DT_DEBUG_LUA,
             "[lua develop] active_modules: no darkroom image loaded"
             " (develop=%p iop=%p), returning empty list\n",
             (void *)dev, (void *)(dev ? dev->iop : NULL));
    lua_newtable(L);
    return 1;
  }

  lua_newtable(L);
  int idx = 1;
  for(GList *iter = dev->iop; iter; iter = g_list_next(iter))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)iter->data;
    if(!module) continue;

    lua_newtable(L);

    lua_pushstring(L, module->op);
    lua_setfield(L, -2, "op");

    lua_pushinteger(L, module->instance);
    lua_setfield(L, -2, "instance");

    lua_pushstring(L, module->multi_name);
    lua_setfield(L, -2, "multi_name");

    lua_pushboolean(L, module->enabled);
    lua_setfield(L, -2, "enabled");

    lua_pushboolean(L, module->have_introspection);
    lua_setfield(L, -2, "has_introspection");

    lua_rawseti(L, -2, idx++);
  }

  return 1;
}

int dt_lua_init_develop(lua_State *L)
{
  dt_lua_push_darktable_lib(L);
  luaA_Type type_id = dt_lua_init_singleton(L, "develop", NULL);
  lua_setfield(L, -2, "develop");
  lua_pop(L, 1);

  // version(): pure constant, no GUI-thread marshaling needed.
  lua_pushcfunction(L, version_cb);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "version");

  // active_modules(): reads live develop state -> wrap onto the GUI thread.
  lua_pushcfunction(L, active_modules_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "active_modules");

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
