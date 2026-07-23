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
#include "common/introspection.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "lua/call.h"
#include "lua/configuration.h"
#include "lua/types.h"
#include <glib.h>
#include <stdarg.h>

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

/* INSTANCE-KEY CONVENTION (used by every method in this namespace)
   ------------------------------------------------------------------
   The PUBLIC api key `instance` == module->multi_priority (imageop.h:290):
   the user/GUI 0-based multi-instance index, 0 = the base instance. This is
   NOT module->instance (imageop.h:182), which is an internal, opaque history
   id (e.g. 89 for the base exposure). The raw internal id is still exposed as
   a separate `id` field for debugging, but all method arguments named
   `instance` -- and the matching logic in get_params() -- mean multi_priority.
   Keep this consistent for all future methods so a round-trip
   active_modules()->get_params() uses the same key. */

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

    // PUBLIC instance key == multi_priority (0-based GUI index, 0 = base).
    lua_pushinteger(L, module->multi_priority);
    lua_setfield(L, -2, "instance");

    // raw internal history id, kept for debugging only (NOT the public key).
    lua_pushinteger(L, module->instance);
    lua_setfield(L, -2, "id");

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

/* Push an error table {error="..."} and return 1. Never raises a Lua error,
   so a bad op / no-darkroom / no-introspection is a graceful result, not a
   crash. */
static int push_error(lua_State *L, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  char *msg = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  lua_newtable(L);
  lua_pushstring(L, msg);
  lua_setfield(L, -2, "error");
  g_free(msg);
  return 1;
}

/* scalar wire shape: {value, min, max, default} as numbers (float/double). */
static void push_num_field(lua_State *L, double value, double min, double max, double def)
{
  lua_newtable(L);
  lua_pushnumber(L, value); lua_setfield(L, -2, "value");
  lua_pushnumber(L, min);   lua_setfield(L, -2, "min");
  lua_pushnumber(L, max);   lua_setfield(L, -2, "max");
  lua_pushnumber(L, def);   lua_setfield(L, -2, "default");
}

/* scalar wire shape for integral fields: value pushed as a Lua integer so it
   JSON-encodes as a plain int, bounds too. */
static void push_int_field(lua_State *L, lua_Integer value, lua_Integer min, lua_Integer max,
                           lua_Integer def)
{
  lua_newtable(L);
  lua_pushinteger(L, value); lua_setfield(L, -2, "value");
  lua_pushinteger(L, min);   lua_setfield(L, -2, "min");
  lua_pushinteger(L, max);   lua_setfield(L, -2, "max");
  lua_pushinteger(L, def);   lua_setfield(L, -2, "default");
}

/* placeholder for types this task does not decode yet (array/struct/opaque):
   {type=<t>, writable=false}. Full decode is a follow-up (PLAN.md §3.1). */
static void push_placeholder(lua_State *L, const char *type)
{
  lua_newtable(L);
  lua_pushstring(L, type); lua_setfield(L, -2, "type");
  lua_pushboolean(L, FALSE); lua_setfield(L, -2, "writable");
}

/* darktable.develop.get_params(op, instance)
   Return a typed field map for the module `op` at multi-instance `instance`
   (== multi_priority, see INSTANCE-KEY CONVENTION above). Walks the module's
   introspection linear field list and reads each live value out of
   module->params at header.offset. Reads live develop/pipe state, so it MUST
   run on the GUI thread -- registered wrapped in dt_lua_gtk_wrap.

   Wire shapes (PLAN.md §3.1, this task scope = scalar+bool+enum):
     float/double/int/uint -> {value,min,max,default}
     bool                   -> bare true/false
     enum                   -> {value,label,options[]}
     array/struct/union/opaque -> {type,writable=false} placeholder (follow-up)

   On any guard failure (bad args, no darkroom, unknown module/instance, no
   introspection) returns a {error="..."} table and never crashes.

   Return: {op, instance, id, fields = { <name> = <shape>, ... }}. */
static int get_params_cb(lua_State *L)
{
  const char *op = luaL_checkstring(L, 1);
  const int instance = luaL_optinteger(L, 2, 0);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  // Match on op AND multi_priority (the public instance key), never on the
  // internal module->instance id.
  dt_iop_module_t *module = NULL;
  for(GList *iter = dev->iop; iter; iter = g_list_next(iter))
  {
    dt_iop_module_t *m = (dt_iop_module_t *)iter->data;
    if(m && !g_strcmp0(m->op, op) && m->multi_priority == instance)
    {
      module = m;
      break;
    }
  }

  if(!module)
    return push_error(L, "no module '%s' instance %d in current darkroom", op, instance);

  if(!module->have_introspection || !module->so || !module->params)
    return push_error(L, "module '%s' has no introspection", op);

  dt_introspection_field_t *fields = module->so->get_introspection_linear();
  if(!fields)
    return push_error(L, "module '%s' introspection returned no fields", op);

  lua_newtable(L); // result

  lua_pushstring(L, module->op);
  lua_setfield(L, -2, "op");
  lua_pushinteger(L, module->multi_priority);
  lua_setfield(L, -2, "instance");
  lua_pushinteger(L, module->instance);
  lua_setfield(L, -2, "id");

  lua_newtable(L); // fields
  const char *const blob = (const char *)module->params;

  for(dt_introspection_field_t *f = fields; f->header.type != DT_INTROSPECTION_TYPE_NONE; f++)
  {
    const char *key = f->header.name ? f->header.name : f->header.field_name;
    if(!key || !*key) continue;
    const void *p = blob + f->header.offset;

    switch(f->header.type)
    {
      case DT_INTROSPECTION_TYPE_FLOAT:
        push_num_field(L, *(const float *)p, f->Float.Min, f->Float.Max, f->Float.Default);
        break;
      case DT_INTROSPECTION_TYPE_DOUBLE:
        push_num_field(L, *(const double *)p, f->Double.Min, f->Double.Max, f->Double.Default);
        break;
      case DT_INTROSPECTION_TYPE_INT:
        push_int_field(L, *(const int *)p, f->Int.Min, f->Int.Max, f->Int.Default);
        break;
      case DT_INTROSPECTION_TYPE_UINT:
        push_int_field(L, *(const unsigned int *)p, f->UInt.Min, f->UInt.Max, f->UInt.Default);
        break;
      case DT_INTROSPECTION_TYPE_BOOL:
        // bare bool per §3.1
        lua_pushboolean(L, *(const gboolean *)p);
        break;
      case DT_INTROSPECTION_TYPE_ENUM:
      {
        const int v = *(const int *)p;
        lua_newtable(L);
        lua_pushinteger(L, v);
        lua_setfield(L, -2, "value");
        const char *label = dt_introspection_get_enum_name(f, v);
        lua_pushstring(L, label ? label : "");
        lua_setfield(L, -2, "label");
        lua_newtable(L); // options: all enum names, in declaration order
        int oi = 1;
        for(dt_introspection_type_enum_tuple_t *it = f->Enum.values; it && it->name; it++)
        {
          lua_pushstring(L, it->name);
          lua_rawseti(L, -2, oi++);
        }
        lua_setfield(L, -2, "options");
        break;
      }
      case DT_INTROSPECTION_TYPE_ARRAY:
        push_placeholder(L, "array");
        break;
      case DT_INTROSPECTION_TYPE_STRUCT:
        push_placeholder(L, "struct");
        break;
      case DT_INTROSPECTION_TYPE_UNION:
        push_placeholder(L, "union");
        break;
      default:
        // OPAQUE, CHAR, complex, and any other not-yet-decoded scalar type
        push_placeholder(L, "opaque");
        break;
    }

    lua_setfield(L, -2, key);
  }

  lua_setfield(L, -2, "fields");
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

  // get_params(op, instance): reads live params/introspection -> GUI thread.
  lua_pushcfunction(L, get_params_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "get_params");

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
