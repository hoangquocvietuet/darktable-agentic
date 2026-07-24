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
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/pixelpipe_hb.h"
#include "lua/call.h"
#include "lua/configuration.h"
#include "lua/lua.h"
#include "lua/types.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <float.h>
#include <png.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>

// API contract version of the darktable.develop bindings. Bump whenever the
// wire shapes (see PLAN.md §3.1) change so the bridge can refuse a mismatch.
// API 2 (T1.7): get_params/set_params decode+encode ARRAY/STRUCT/UNION
// composites (curves etc.); adds enable_module. API 1 was scalar/bool/enum only.
// API 3 (T1.4): robust enable_module (forces a distinct history item + flushes
// the pixelpipe cache via dt_dev_pixelpipe_rebuild to kill the enable/in-flight
// -render cache-poison no-op race); adds add_instance(op).
// API 4 (FIX 1-3): set_params/enable_module now refresh the module GUI widgets
// via dt_iop_gui_update after the commit (slider was stale); preview() accepts
// an optional normalized region (x,y,w,h) to crop the backbuf before downscale
// and echoes {frame_width,frame_height[,region]}; adds get_viewport() returning
// the main + preview2 canvas zoom/pan state.
// API 5 (T2.2): adds add_path_mask(op, instance, points, opacity[, feather]) --
// builds a DT_MASKS_PATH form from a normalized polygon, wires it to the
// module's blend group via dt_masks_gui_form_save_creation, and turns on the
// drawn-mask blend mode so the module's effect is restricted to the region.
// API 6 (T2.4): get_viewport() now adds a ready-to-use `region` {x,y,w,h} to
// each viewport (main, and preview2 when active) -- the visible sub-rectangle of
// the full image in TOP-LEFT normalized 0..1 coords, CLAMPED to [0,1]. It is a
// drop-in for get_preview()'s `region` arg; the raw center-relative zoom_x/
// zoom_y (which can be negative) are NOT. Purely additive: all API-5 fields kept.
#define DT_LUA_DEVELOP_API 6
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

/* Resolve a live module by op + multi_priority (the PUBLIC instance key, see
   INSTANCE-KEY CONVENTION). Returns NULL if no darkroom image or no match.
   Shared by every method that targets a single module. */
static dt_iop_module_t *_find_module(dt_develop_t *dev, const char *op, int instance)
{
  if(!dev || !dev->iop) return NULL;
  for(GList *iter = dev->iop; iter; iter = g_list_next(iter))
  {
    dt_iop_module_t *m = (dt_iop_module_t *)iter->data;
    if(m && !g_strcmp0(m->op, op) && m->multi_priority == instance)
      return m;
  }
  return NULL;
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

/* placeholder for types we still cannot decode (opaque/complex/too-deep):
   {type=<t>, writable=false}. */
static void push_placeholder(lua_State *L, const char *type)
{
  lua_newtable(L);
  lua_pushstring(L, type); lua_setfield(L, -2, "type");
  lua_pushboolean(L, FALSE); lua_setfield(L, -2, "writable");
}

/* Recursion depth guard shared by the composite decode (get) and encode (set)
   walkers. darktable's deepest generated params is the tonecurve 2D
   array-of-struct (array -> array -> struct -> float = 4 levels); 8 leaves
   generous headroom while still bounding any pathological/cyclic descriptor. */
#define DT_LUA_DEVELOP_MAX_DEPTH 8

/* Push the scalar wire shape for a leaf introspection field read from `p`.
   `bare` selects the CONTEXT-dependent shape (PLAN.md §3.1):
     bare == FALSE  top-level field  -> rich object {value,min,max,default}
                    (enum -> {value,label,options}); bool stays a bare bool.
     bare == TRUE   element/member inside an array or struct -> just the bare
                    number / bool / enum-integer, so an array of scalars is a
                    JSON array of numbers and a curve node is {x=..,y=..}.
   Pushes exactly one value. Unknown scalar types -> {type=opaque}. */
static void decode_scalar(lua_State *L, dt_introspection_field_t *f, const void *p, gboolean bare)
{
  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:
      if(bare) lua_pushnumber(L, *(const float *)p);
      else push_num_field(L, *(const float *)p, f->Float.Min, f->Float.Max, f->Float.Default);
      break;
    case DT_INTROSPECTION_TYPE_DOUBLE:
      if(bare) lua_pushnumber(L, *(const double *)p);
      else push_num_field(L, *(const double *)p, f->Double.Min, f->Double.Max, f->Double.Default);
      break;
    case DT_INTROSPECTION_TYPE_INT:
      if(bare) lua_pushinteger(L, *(const int *)p);
      else push_int_field(L, *(const int *)p, f->Int.Min, f->Int.Max, f->Int.Default);
      break;
    case DT_INTROSPECTION_TYPE_UINT:
      if(bare) lua_pushinteger(L, *(const unsigned int *)p);
      else push_int_field(L, *(const unsigned int *)p, f->UInt.Min, f->UInt.Max, f->UInt.Default);
      break;
    case DT_INTROSPECTION_TYPE_BOOL:
      lua_pushboolean(L, *(const gboolean *)p);
      break;
    case DT_INTROSPECTION_TYPE_ENUM:
    {
      const int v = *(const int *)p;
      if(bare) { lua_pushinteger(L, v); break; }
      lua_newtable(L);
      lua_pushinteger(L, v); lua_setfield(L, -2, "value");
      const char *label = dt_introspection_get_enum_name(f, v);
      lua_pushstring(L, label ? label : ""); lua_setfield(L, -2, "label");
      lua_newtable(L);
      int oi = 1;
      for(dt_introspection_type_enum_tuple_t *it = f->Enum.values; it && it->name; it++)
      {
        lua_pushstring(L, it->name);
        lua_rawseti(L, -2, oi++);
      }
      lua_setfield(L, -2, "options");
      break;
    }
    default:
      push_placeholder(L, "opaque");
      break;
  }
}

/* Recursively decode ANY introspection field at blob pointer `p` and push its
   JSON-shaped value (PLAN.md §3.1). Composites recurse via the header helpers:
     ARRAY  -> Lua array, elements at dt_introspection_access_array().
               A CHAR array is emitted as a Lua string (C string field).
     STRUCT/UNION -> Lua object keyed by each child's field_name, child pointer
               computed as p + (child.offset - self.offset) -- the same
               relative-offset math dt_introspection_get_child() uses.
   Elements/members are decoded `bare` (numbers/objects without min/max), so a
   scalar array is [n,n,..] and a struct/curve node is {x=..,y=..}. Depth-bounded
   by DT_LUA_DEVELOP_MAX_DEPTH; over the limit a {type=too_deep} placeholder is
   pushed instead of recursing. Pushes exactly one value. */
static void decode_field(lua_State *L, dt_introspection_field_t *f, const void *p, int depth,
                         gboolean bare)
{
  luaL_checkstack(L, 6, "get_params decode");
  if(depth > DT_LUA_DEVELOP_MAX_DEPTH) { push_placeholder(L, "too_deep"); return; }

  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_ARRAY:
    {
      // a char[] is a C string -> emit as one Lua string, not an array of bytes
      if(f->Array.type == DT_INTROSPECTION_TYPE_CHAR)
      {
        const char *s = (const char *)p;
        lua_pushlstring(L, s, strnlen(s, f->Array.count));
        break;
      }
      lua_newtable(L);
      for(unsigned int e = 0; e < f->Array.count; e++)
      {
        dt_introspection_field_t *child = NULL;
        void *ep = dt_introspection_access_array(f, (void *)p, e, &child);
        if(!ep || !child) lua_pushnil(L);
        else decode_field(L, child, ep, depth + 1, TRUE);
        lua_rawseti(L, -2, (int)e + 1);
      }
      break;
    }
    case DT_INTROSPECTION_TYPE_STRUCT:
    case DT_INTROSPECTION_TYPE_UNION:
    {
      dt_introspection_field_t **it =
        (f->header.type == DT_INTROSPECTION_TYPE_STRUCT) ? f->Struct.fields : f->Union.fields;
      lua_newtable(L);
      for(; it && *it; it++)
      {
        dt_introspection_field_t *c = *it;
        const char *cname = c->header.field_name;
        if(!cname || !*cname) continue;
        const void *cp = (const char *)p + (c->header.offset - f->header.offset);
        decode_field(L, c, cp, depth + 1, TRUE);
        lua_setfield(L, -2, cname);
      }
      break;
    }
    default:
      decode_scalar(L, f, p, bare);
      break;
  }
}

/* Resolve the TOP-LEVEL params fields of a module = the children of the root
   params struct (get_introspection()->field). The linear list from
   get_introspection_linear() is a POST-ORDER flatten that ALSO contains every
   composite's element/member descriptors (e.g. `tonecurve[0][0].x`,
   `tonecurve[0]`) as separate dotted/bracketed leaves, so it is NOT the right
   thing to iterate for a top-level field map. The root struct's `fields[]`
   gives exactly the user-facing fields, each of which we then decode/encode
   recursively. Returns the root STRUCT descriptor, or NULL. */
static dt_introspection_field_t *root_struct(dt_iop_module_t *module)
{
  if(!module->so->get_introspection) return NULL;
  dt_introspection_t *in = module->so->get_introspection();
  if(!in || !in->field) return NULL;
  if(in->field->header.type != DT_INTROSPECTION_TYPE_STRUCT) return NULL;
  return in->field;
}

/* Find a top-level field by its public name among the root struct children. */
static dt_introspection_field_t *find_toplevel(dt_iop_module_t *module, const char *name)
{
  dt_introspection_field_t *root = root_struct(module);
  if(!root) return NULL;
  for(dt_introspection_field_t **it = root->Struct.fields; it && *it; it++)
  {
    const char *k = (*it)->header.name ? (*it)->header.name : (*it)->header.field_name;
    if(k && !g_strcmp0(k, name)) return *it;
  }
  return NULL;
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

  dt_introspection_field_t *root = root_struct(module);
  if(!root)
    return push_error(L, "module '%s' introspection has no root struct", op);

  lua_newtable(L); // result

  lua_pushstring(L, module->op);
  lua_setfield(L, -2, "op");
  lua_pushinteger(L, module->multi_priority);
  lua_setfield(L, -2, "instance");
  lua_pushinteger(L, module->instance);
  lua_setfield(L, -2, "id");

  lua_newtable(L); // fields
  const char *const blob = (const char *)module->params;

  // Iterate the TOP-LEVEL fields (root struct children), decoding each
  // recursively. Scalars are decoded rich ({value,min,max,default}); ARRAY /
  // STRUCT fields recurse into nested arrays/objects (see decode_field). This
  // deliberately does NOT walk get_introspection_linear() -- that flat list
  // also contains every composite's sub-element leaves (see root_struct note).
  for(dt_introspection_field_t **it = root->Struct.fields; it && *it; it++)
  {
    dt_introspection_field_t *f = *it;
    const char *key = f->header.name ? f->header.name : f->header.field_name;
    if(!key || !*key) continue;
    const void *p = blob + f->header.offset;
    decode_field(L, f, p, 0, FALSE);
    lua_setfield(L, -2, key);
  }

  lua_setfield(L, -2, "fields");
  return 1;
}

/* Append one clamp record {field,requested,applied,min,max} (numeric bounds)
   to the array table sitting at absolute stack index `arr`, at position `n`.
   Pushes a fresh table and pops it via lua_rawseti, leaving the stack as it
   was found. */
static void push_clamp_num(lua_State *L, int arr, int n, const char *field,
                           double req, double ap, double mn, double mx)
{
  lua_newtable(L);
  lua_pushstring(L, field);  lua_setfield(L, -2, "field");
  lua_pushnumber(L, req);    lua_setfield(L, -2, "requested");
  lua_pushnumber(L, ap);     lua_setfield(L, -2, "applied");
  lua_pushnumber(L, mn);     lua_setfield(L, -2, "min");
  lua_pushnumber(L, mx);     lua_setfield(L, -2, "max");
  lua_rawseti(L, arr, n);
}

/* Integer-typed clamp record (requested/applied/min/max as Lua integers so
   they JSON-encode as plain ints). */
static void push_clamp_int(lua_State *L, int arr, int n, const char *field,
                           lua_Integer req, lua_Integer ap, lua_Integer mn, lua_Integer mx)
{
  lua_newtable(L);
  lua_pushstring(L, field);   lua_setfield(L, -2, "field");
  lua_pushinteger(L, req);    lua_setfield(L, -2, "requested");
  lua_pushinteger(L, ap);     lua_setfield(L, -2, "applied");
  lua_pushinteger(L, mn);     lua_setfield(L, -2, "min");
  lua_pushinteger(L, mx);     lua_setfield(L, -2, "max");
  lua_rawseti(L, arr, n);
}

/* Recursively ENCODE a Lua value at absolute stack index `vidx` into ANY
   introspection field at blob pointer `p` (the set-side mirror of
   decode_field; PLAN.md §3.1). Writes happen directly at computed offsets.

   Shapes accepted (same as get emits):
     scalar leaf : bare number / bool / enum(int|label). Clamped to the field's
                   introspection Min/Max exactly like the top-level scalar path;
                   each clamp appended to CLAMPED as {field=<path>,requested,
                   applied,min,max}. An out-of-set enum is skipped (not written).
     ARRAY       : a Lua array. Elements are written element-wise. A PARTIAL
                   array is allowed (fewer elements than capacity -> the rest of
                   the blob is left untouched), which is how a single curve
                   channel is set without disturbing the others. OVER-CAPACITY
                   is the guard (acceptance #4): if the caller supplies MORE
                   elements than Array.count, only the first Array.count are
                   written (never past the array -- no overflow) and one
                   {field=<path>,requested=<n>,applied=<cap>,note="array capacity
                   exceeded"} record is reported. A char[] target accepts a Lua
                   string.
     STRUCT/UNION: a Lua object. Only the members present in the table are
                   written (missing members left untouched); child pointer is
                   p + (child.offset - self.offset).

   `path` is the dotted/bracketed field path used purely for clamp reporting
   (e.g. "tonecurve[0][3].y"). Sets *wrote = TRUE if any leaf was written.
   Depth-bounded by DT_LUA_DEVELOP_MAX_DEPTH. Never raises; a wrong-shaped value
   (e.g. a number where an array is expected) is simply ignored. */
static void encode_field(lua_State *L, dt_introspection_field_t *f, void *p, int vidx,
                         int depth, const char *path, int CLAMPED, int *clamped_n,
                         gboolean *wrote)
{
  luaL_checkstack(L, 8, "set_params encode");
  if(depth > DT_LUA_DEVELOP_MAX_DEPTH) return;

  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_ARRAY:
    {
      // char[] target fed a Lua string: copy bytes (NUL-pad/truncate to cap).
      if(f->Array.type == DT_INTROSPECTION_TYPE_CHAR && lua_type(L, vidx) == LUA_TSTRING)
      {
        size_t slen = 0;
        const char *s = lua_tolstring(L, vidx, &slen);
        const size_t cap = f->Array.count;
        const size_t n = slen < cap ? slen : cap;
        memcpy(p, s, n);
        if(n < cap) ((char *)p)[n] = '\0';
        *wrote = TRUE;
        break;
      }
      if(lua_type(L, vidx) != LUA_TTABLE) break; // wrong shape -> ignore
      const size_t provided = lua_rawlen(L, vidx);
      const size_t cap = f->Array.count;
      const size_t n = provided < cap ? provided : cap;
      if(provided > cap)
      {
        // over-capacity guard: report, write only up to capacity.
        lua_newtable(L);
        lua_pushstring(L, path);                 lua_setfield(L, -2, "field");
        lua_pushinteger(L, (lua_Integer)provided); lua_setfield(L, -2, "requested");
        lua_pushinteger(L, (lua_Integer)cap);    lua_setfield(L, -2, "applied");
        lua_pushstring(L, "array capacity exceeded"); lua_setfield(L, -2, "note");
        lua_rawseti(L, CLAMPED, ++(*clamped_n));
      }
      for(size_t e = 0; e < n; e++)
      {
        dt_introspection_field_t *child = NULL;
        void *ep = dt_introspection_access_array(f, p, (unsigned int)e, &child);
        if(!ep || !child) continue;
        lua_rawgeti(L, vidx, (int)e + 1);
        const int evidx = lua_gettop(L);
        if(!lua_isnil(L, evidx))
        {
          char *cp = g_strdup_printf("%s[%zu]", path, e);
          encode_field(L, child, ep, evidx, depth + 1, cp, CLAMPED, clamped_n, wrote);
          g_free(cp);
        }
        lua_pop(L, 1);
      }
      break;
    }
    case DT_INTROSPECTION_TYPE_STRUCT:
    case DT_INTROSPECTION_TYPE_UNION:
    {
      if(lua_type(L, vidx) != LUA_TTABLE) break;
      dt_introspection_field_t **it =
        (f->header.type == DT_INTROSPECTION_TYPE_STRUCT) ? f->Struct.fields : f->Union.fields;
      for(; it && *it; it++)
      {
        dt_introspection_field_t *c = *it;
        const char *cname = c->header.field_name;
        if(!cname || !*cname) continue;
        lua_getfield(L, vidx, cname);
        const int cvidx = lua_gettop(L);
        if(!lua_isnil(L, cvidx))
        {
          void *cp = (char *)p + (c->header.offset - f->header.offset);
          char *np = g_strdup_printf("%s.%s", path, cname);
          encode_field(L, c, cp, cvidx, depth + 1, np, CLAMPED, clamped_n, wrote);
          g_free(np);
        }
        lua_pop(L, 1);
      }
      break;
    }
    case DT_INTROSPECTION_TYPE_FLOAT:
    {
      const double req = lua_tonumber(L, vidx);
      const double mn = f->Float.Min, mx = f->Float.Max;
      double ap = req; gboolean cl = FALSE;
      if(ap < mn) { ap = mn; cl = TRUE; }
      if(ap > mx) { ap = mx; cl = TRUE; }
      *(float *)p = (float)ap; *wrote = TRUE;
      if(cl) push_clamp_num(L, CLAMPED, ++(*clamped_n), path, req, ap, mn, mx);
      break;
    }
    case DT_INTROSPECTION_TYPE_DOUBLE:
    {
      const double req = lua_tonumber(L, vidx);
      const double mn = f->Double.Min, mx = f->Double.Max;
      double ap = req; gboolean cl = FALSE;
      if(ap < mn) { ap = mn; cl = TRUE; }
      if(ap > mx) { ap = mx; cl = TRUE; }
      *(double *)p = ap; *wrote = TRUE;
      if(cl) push_clamp_num(L, CLAMPED, ++(*clamped_n), path, req, ap, mn, mx);
      break;
    }
    case DT_INTROSPECTION_TYPE_INT:
    {
      const lua_Integer req = (lua_Integer)llround(lua_tonumber(L, vidx));
      const lua_Integer mn = f->Int.Min, mx = f->Int.Max;
      lua_Integer ap = req; gboolean cl = FALSE;
      if(ap < mn) { ap = mn; cl = TRUE; }
      if(ap > mx) { ap = mx; cl = TRUE; }
      *(int *)p = (int)ap; *wrote = TRUE;
      if(cl) push_clamp_int(L, CLAMPED, ++(*clamped_n), path, req, ap, mn, mx);
      break;
    }
    case DT_INTROSPECTION_TYPE_UINT:
    {
      const lua_Integer req = (lua_Integer)llround(lua_tonumber(L, vidx));
      const lua_Integer mn = (lua_Integer)f->UInt.Min, mx = (lua_Integer)f->UInt.Max;
      lua_Integer ap = req; gboolean cl = FALSE;
      if(ap < mn) { ap = mn; cl = TRUE; }
      if(ap > mx) { ap = mx; cl = TRUE; }
      *(unsigned int *)p = (unsigned int)ap; *wrote = TRUE;
      if(cl) push_clamp_int(L, CLAMPED, ++(*clamped_n), path, req, ap, mn, mx);
      break;
    }
    case DT_INTROSPECTION_TYPE_BOOL:
      *(gboolean *)p = lua_toboolean(L, vidx) ? 1 : 0; *wrote = TRUE;
      break;
    case DT_INTROSPECTION_TYPE_ENUM:
    {
      int val = 0; gboolean ok = FALSE;
      if(lua_type(L, vidx) == LUA_TSTRING)
        ok = dt_introspection_get_enum_value(f, lua_tostring(L, vidx), &val);
      else if(lua_isnumber(L, vidx))
      {
        val = (int)lua_tointeger(L, vidx);
        ok = (dt_introspection_get_enum_name(f, val) != NULL);
      }
      if(ok) { *(int *)p = val; *wrote = TRUE; }
      else
      {
        lua_newtable(L);
        lua_pushstring(L, path);                  lua_setfield(L, -2, "field");
        lua_pushvalue(L, vidx);                   lua_setfield(L, -2, "requested");
        lua_pushstring(L, "unknown enum value");  lua_setfield(L, -2, "note");
        lua_rawseti(L, CLAMPED, ++(*clamped_n));
      }
      break;
    }
    default:
      break; // opaque / complex nested leaf -> silently skip
  }
}

/* darktable.develop.set_params(op, instance, {field=val, ...})
   ------------------------------------------------------------------
   Write one or more scalar/bool/enum fields into the live params blob of the
   module `op` at multi-instance `instance` (== multi_priority, see
   INSTANCE-KEY CONVENTION), clamp each numeric value to its introspection
   Min/Max, then commit the change to history and trigger a live reprocess.

   THREADING / LOCKING REASONING (Risk #1 in PLAN.md -- the whole point of
   this task; read before touching):
     * This callback is registered wrapped in dt_lua_gtk_wrap() (see
       dt_lua_init_develop below). gtk_wrap marshals the ENTIRE callback onto
       darktable's GUI main thread and blocks the calling Lua thread until it
       returns. Therefore every line here runs ON THE GUI THREAD.
     * dt_dev_add_history_item() (develop.c:1445) is the GUI commit path. Its
       internal _dev_add_history_item (develop.c:1382) acquires
       dev->history_mutex itself (develop.c:1399) and releases it
       (develop.c:1424) BEFORE raising any signal / queueing the redraw. It
       must run on the GUI thread and must NOT be called while the caller
       already holds history_mutex.
     * We satisfy both invariants by construction: we are already on the GUI
       thread (gtk_wrap), and we NEVER take history_mutex ourselves -- we only
       touch module->params (plain memory writes, same as a slider's value
       callback does before committing) and then hand off to
       dt_dev_add_history_item, which owns the lock for the whole critical
       section. No lock is held across the call, so no lock-ordering inversion
       and no double-lock is possible. We also do NOT nest another
       dt_lua_gtk_wrap and do NOT g_main_context_invoke -- doing so from the
       GUI thread would deadlock waiting on the main loop we are running on.
     * DT_IN_GUI_UPDATE() guard (develop.c:1388): _dev_add_history_item
       early-returns (does nothing) if !darktable.gui || DT_IN_GUI_UPDATE().
       DT_IN_GUI_UPDATE() is true only inside a DT_ENTER_GUI_UPDATE/LEAVE
       region (a widget being programmatically synced). Our gtk_wrap callback
       runs directly off the main loop, not inside such a region, so the guard
       passes and the write is committed. If a future change makes the commit
       silently no-op, this guard is the first suspect -- surface it, do not
       work around it.

   Params are written directly through the module's generated
   so->get_p(module->params, name) pointer, cast by the field's introspection
   type -- the SAME name/offset contract get_params() reads, so a
   set_params -> get_params round-trip is consistent by construction.

   Return (PLAN.md §3.2): {ok=true, applied={f=v,...},
     clamped={{field,requested,applied,min,max}, ...},
     unknown_fields={...}}. Out-of-range values are clamped and reported,
   never rejected. Unknown field names are ignored and listed. An out-of-set
   enum value is reported (with a note) and NOT written (no garbage). Unknown
   module / not-in-darkroom / no-introspection -> {error="..."}. */
static int set_params_cb(lua_State *L)
{
  const char *op = luaL_checkstring(L, 1);
  const int instance = luaL_optinteger(L, 2, 0);
  luaL_checktype(L, 3, LUA_TTABLE);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  // Match on op AND multi_priority (the public instance key), like get_params.
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

  if(!module->have_introspection || !module->so || !module->params
     || !module->so->get_introspection || !module->so->get_p)
    return push_error(L, "module '%s' has no introspection", op);

  if(!root_struct(module))
    return push_error(L, "module '%s' introspection has no root struct", op);

  // Three result sub-tables kept at fixed ABSOLUTE stack indices for the whole
  // iteration, assembled into the final result only at the end. Keeping them
  // below the lua_next key/value pair keeps the iteration stack disciplined.
  lua_newtable(L); const int APPLIED = lua_gettop(L);
  lua_newtable(L); const int CLAMPED = lua_gettop(L);
  lua_newtable(L); const int UNKNOWN = lua_gettop(L);
  int clamped_n = 0, unknown_n = 0;
  gboolean any_written = FALSE;

  lua_pushnil(L);
  while(lua_next(L, 3) != 0)
  {
    // Make the per-field stack budget explicit (T1.3 review): each iteration
    // may push a fetched .value, a clamp/unknown record table plus its several
    // string/number members on top of the key/value already held. 8 slots
    // covers the deepest path (enum clamp record) with margin; luaL_checkstack
    // raises a clean Lua error rather than risking a silent overflow.
    luaL_checkstack(L, 8, "set_params");
    // stack now: ..., key (-2), value (-1)
    if(lua_type(L, -2) != LUA_TSTRING)
    {
      lua_pop(L, 1); // drop value, keep key for next lua_next
      continue;
    }
    const char *fname = lua_tostring(L, -2);

    // Resolve against the TOP-LEVEL fields (root struct children), the same set
    // get_params exposes -- never against the flattened linear leaves.
    dt_introspection_field_t *fld = find_toplevel(module, fname);
    if(!fld)
    {
      lua_pushstring(L, fname);
      lua_rawseti(L, UNKNOWN, ++unknown_n);
      lua_pop(L, 1); // drop value, keep key
      continue;
    }

    const int vidx = lua_gettop(L); // the raw value lua_next returned

    // COMPOSITE fields (array/struct/union incl. curves) are written by the
    // recursive encoder against the field's blob base, then the post-write blob
    // is decoded back as the "applied" value. Handled before the scalar-object
    // unwrap because their value IS the array/object itself, not a {value=..}.
    if(fld->header.type == DT_INTROSPECTION_TYPE_ARRAY
       || fld->header.type == DT_INTROSPECTION_TYPE_STRUCT
       || fld->header.type == DT_INTROSPECTION_TYPE_UNION)
    {
      void *base = (char *)module->params + fld->header.offset;
      gboolean wrote = FALSE;
      encode_field(L, fld, base, vidx, 0, fname, CLAMPED, &clamped_n, &wrote);
      if(wrote)
      {
        any_written = TRUE;
        decode_field(L, fld, base, 0, FALSE); // snapshot of the new value
        lua_setfield(L, APPLIED, fname);
      }
      else { lua_pushstring(L, fname); lua_rawseti(L, UNKNOWN, ++unknown_n); }
      lua_pop(L, 1); // drop the iteration value, keep key for lua_next
      continue;
    }

    // Scalars: accept either a bare scalar/bool/string OR the object form
    // {value=...} (get_params' read shape). If object, fetch .value onto the
    // stack top and pop it after use so the iteration value stays intact.
    const gboolean is_obj = (lua_type(L, vidx) == LUA_TTABLE);
    int rvidx = vidx;
    if(is_obj)
    {
      lua_getfield(L, vidx, "value");
      rvidx = lua_gettop(L);
    }

    switch(fld->header.type)
    {
      case DT_INTROSPECTION_TYPE_FLOAT:
      {
        const double req = lua_tonumber(L, rvidx);
        const double mn = fld->Float.Min, mx = fld->Float.Max;
        double ap = req; gboolean cl = FALSE;
        if(ap < mn) { ap = mn; cl = TRUE; }
        if(ap > mx) { ap = mx; cl = TRUE; }
        float *wp = (float *)module->so->get_p(module->params, fname);
        if(wp)
        {
          *wp = (float)ap;
          any_written = TRUE;
          lua_pushnumber(L, ap); lua_setfield(L, APPLIED, fname);
          if(cl) push_clamp_num(L, CLAMPED, ++clamped_n, fname, req, ap, mn, mx);
        }
        else { lua_pushstring(L, fname); lua_rawseti(L, UNKNOWN, ++unknown_n); }
        break;
      }
      case DT_INTROSPECTION_TYPE_DOUBLE:
      {
        const double req = lua_tonumber(L, rvidx);
        const double mn = fld->Double.Min, mx = fld->Double.Max;
        double ap = req; gboolean cl = FALSE;
        if(ap < mn) { ap = mn; cl = TRUE; }
        if(ap > mx) { ap = mx; cl = TRUE; }
        double *wp = (double *)module->so->get_p(module->params, fname);
        if(wp)
        {
          *wp = ap;
          any_written = TRUE;
          lua_pushnumber(L, ap); lua_setfield(L, APPLIED, fname);
          if(cl) push_clamp_num(L, CLAMPED, ++clamped_n, fname, req, ap, mn, mx);
        }
        else { lua_pushstring(L, fname); lua_rawseti(L, UNKNOWN, ++unknown_n); }
        break;
      }
      case DT_INTROSPECTION_TYPE_INT:
      {
        const lua_Integer req = (lua_Integer)llround(lua_tonumber(L, rvidx));
        const lua_Integer mn = fld->Int.Min, mx = fld->Int.Max;
        lua_Integer ap = req; gboolean cl = FALSE;
        if(ap < mn) { ap = mn; cl = TRUE; }
        if(ap > mx) { ap = mx; cl = TRUE; }
        int *wp = (int *)module->so->get_p(module->params, fname);
        if(wp)
        {
          *wp = (int)ap;
          any_written = TRUE;
          lua_pushinteger(L, ap); lua_setfield(L, APPLIED, fname);
          if(cl) push_clamp_int(L, CLAMPED, ++clamped_n, fname, req, ap, mn, mx);
        }
        else { lua_pushstring(L, fname); lua_rawseti(L, UNKNOWN, ++unknown_n); }
        break;
      }
      case DT_INTROSPECTION_TYPE_UINT:
      {
        const lua_Integer req = (lua_Integer)llround(lua_tonumber(L, rvidx));
        const lua_Integer mn = (lua_Integer)fld->UInt.Min, mx = (lua_Integer)fld->UInt.Max;
        lua_Integer ap = req; gboolean cl = FALSE;
        if(ap < mn) { ap = mn; cl = TRUE; }
        if(ap > mx) { ap = mx; cl = TRUE; }
        unsigned int *wp = (unsigned int *)module->so->get_p(module->params, fname);
        if(wp)
        {
          *wp = (unsigned int)ap;
          any_written = TRUE;
          lua_pushinteger(L, ap); lua_setfield(L, APPLIED, fname);
          if(cl) push_clamp_int(L, CLAMPED, ++clamped_n, fname, req, ap, mn, mx);
        }
        else { lua_pushstring(L, fname); lua_rawseti(L, UNKNOWN, ++unknown_n); }
        break;
      }
      case DT_INTROSPECTION_TYPE_BOOL:
      {
        const int b = lua_toboolean(L, rvidx) ? 1 : 0; // coerce to 0/1
        gboolean *wp = (gboolean *)module->so->get_p(module->params, fname);
        if(wp)
        {
          *wp = b;
          any_written = TRUE;
          lua_pushboolean(L, b); lua_setfield(L, APPLIED, fname);
        }
        else { lua_pushstring(L, fname); lua_rawseti(L, UNKNOWN, ++unknown_n); }
        break;
      }
      case DT_INTROSPECTION_TYPE_ENUM:
      {
        // accept the integer value OR the symbolic label string; reject (report
        // + do not write) any value not in the enum set.
        int val = 0; gboolean ok = FALSE;
        if(lua_type(L, rvidx) == LUA_TSTRING)
          ok = dt_introspection_get_enum_value(fld, lua_tostring(L, rvidx), &val);
        else if(lua_isnumber(L, rvidx))
        {
          val = (int)lua_tointeger(L, rvidx);
          ok = (dt_introspection_get_enum_name(fld, val) != NULL);
        }
        if(ok)
        {
          int *wp = (int *)module->so->get_p(module->params, fname);
          if(wp)
          {
            *wp = val;
            any_written = TRUE;
            lua_pushinteger(L, val); lua_setfield(L, APPLIED, fname);
          }
          else { lua_pushstring(L, fname); lua_rawseti(L, UNKNOWN, ++unknown_n); }
        }
        else
        {
          // out-of-set enum -> clamp report with a note, NOT written.
          lua_newtable(L);
          lua_pushstring(L, fname);              lua_setfield(L, -2, "field");
          lua_pushvalue(L, rvidx);               lua_setfield(L, -2, "requested");
          lua_pushstring(L, "unknown enum value"); lua_setfield(L, -2, "note");
          lua_rawseti(L, CLAMPED, ++clamped_n);
        }
        break;
      }
      default:
        // opaque / complex / other not-writable scalar leaf (ARRAY/STRUCT/UNION
        // are handled above): known field but unwritable -> report as unknown.
        lua_pushstring(L, fname);
        lua_rawseti(L, UNKNOWN, ++unknown_n);
        break;
    }

    if(is_obj) lua_pop(L, 1); // drop the fetched .value
    lua_pop(L, 1);            // drop the iteration value, keep key for lua_next
  }

  // COMMIT + LIVE REPROCESS. We are on the GUI thread (gtk_wrap); this call
  // takes and releases dev->history_mutex internally and invalidates the pipe.
  // Skip it entirely if nothing was actually written (all fields unknown), to
  // avoid churning an empty history entry. Pass module->enabled per PLAN.md
  // (set_params does NOT force-enable; toggling is enable_module's job, T1.4).
  if(any_written)
  {
    dt_dev_add_history_item(dev, module, module->enabled);

    // GUI WIDGET REFRESH (FIX 1). dt_dev_add_history_item commits params +
    // reprocesses, but does NOT re-sync the module panel's bauhaus widgets --
    // the slider keeps showing the OLD value until the module is reopened. We
    // are already on the GUI thread (gtk_wrap), the only place GUI widget sets
    // may run, so we re-push params into the widgets here.
    //   * dt_iop_gui_update (imageop.c:2260) drives dt_bauhaus_update_from_field
    //     + module->gui_update + blending/expander/header refresh -- all
    //     synchronous GtkWidget/bauhaus sets. It does NOT call back into the
    //     history-add path, so no re-entrancy into our just-released
    //     history_mutex and no deadlock. It wraps its body in
    //     DT_ENTER/LEAVE_GUI_UPDATE, but we run it AFTER the commit, so the
    //     DT_IN_GUI_UPDATE guard in _dev_add_history_item never blocks our own
    //     write (that already happened).
    //   * Guard on module->expander: it is NULL until the darkroom expander is
    //     built, so this is a safe no-op for a module whose GUI is not present
    //     (headless/lighttable-adjacent). dt_iop_gui_update itself also
    //     null-guards gui_data/header/off internally, making this belt-and-
    //     suspenders.
    if(module->expander) dt_iop_gui_update(module);
  }

  // assemble the result table on top of the stack.
  lua_newtable(L);
  lua_pushboolean(L, TRUE);   lua_setfield(L, -2, "ok");
  lua_pushvalue(L, APPLIED);  lua_setfield(L, -2, "applied");
  lua_pushvalue(L, CLAMPED);  lua_setfield(L, -2, "clamped");
  lua_pushvalue(L, UNKNOWN);  lua_setfield(L, -2, "unknown_fields");
  return 1;
}

/* darktable.develop.dump_introspection(op, instance)
   ------------------------------------------------------------------
   Read-only diagnostic used to VERIFY empirically how get_introspection_linear()
   represents composite (array/struct) fields (T1.7 investigation). Returns the
   RAW flat linear list -- each entry's {name, field_name, type, type_name,
   offset, size, array_count, array_elem_type} -- exactly as the generated
   introspection emits it, plus the root struct's top-level child names. This is
   what the report's "how the linear list represents composites" claim is based
   on. Runs on the GUI thread (gtk_wrap). */
static const char *_introspection_type_name(dt_introspection_type_t t)
{
  switch(t)
  {
    case DT_INTROSPECTION_TYPE_NONE: return "NONE";
    case DT_INTROSPECTION_TYPE_OPAQUE: return "OPAQUE";
    case DT_INTROSPECTION_TYPE_FLOAT: return "FLOAT";
    case DT_INTROSPECTION_TYPE_DOUBLE: return "DOUBLE";
    case DT_INTROSPECTION_TYPE_FLOATCOMPLEX: return "FLOATCOMPLEX";
    case DT_INTROSPECTION_TYPE_CHAR: return "CHAR";
    case DT_INTROSPECTION_TYPE_INT8: return "INT8";
    case DT_INTROSPECTION_TYPE_UINT8: return "UINT8";
    case DT_INTROSPECTION_TYPE_SHORT: return "SHORT";
    case DT_INTROSPECTION_TYPE_USHORT: return "USHORT";
    case DT_INTROSPECTION_TYPE_INT: return "INT";
    case DT_INTROSPECTION_TYPE_UINT: return "UINT";
    case DT_INTROSPECTION_TYPE_LONG: return "LONG";
    case DT_INTROSPECTION_TYPE_ULONG: return "ULONG";
    case DT_INTROSPECTION_TYPE_BOOL: return "BOOL";
    case DT_INTROSPECTION_TYPE_ARRAY: return "ARRAY";
    case DT_INTROSPECTION_TYPE_ENUM: return "ENUM";
    case DT_INTROSPECTION_TYPE_STRUCT: return "STRUCT";
    case DT_INTROSPECTION_TYPE_UNION: return "UNION";
    default: return "?";
  }
}

static int dump_introspection_cb(lua_State *L)
{
  const char *op = luaL_checkstring(L, 1);
  const int instance = luaL_optinteger(L, 2, 0);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  dt_iop_module_t *module = NULL;
  for(GList *iter = dev->iop; iter; iter = g_list_next(iter))
  {
    dt_iop_module_t *m = (dt_iop_module_t *)iter->data;
    if(m && !g_strcmp0(m->op, op) && m->multi_priority == instance) { module = m; break; }
  }
  if(!module) return push_error(L, "no module '%s' instance %d", op, instance);
  if(!module->have_introspection || !module->so || !module->so->get_introspection_linear)
    return push_error(L, "module '%s' has no introspection", op);

  lua_newtable(L); // result
  lua_newtable(L); // linear[]
  int i = 1;
  for(dt_introspection_field_t *f = module->so->get_introspection_linear();
      f->header.type != DT_INTROSPECTION_TYPE_NONE; f++)
  {
    lua_newtable(L);
    lua_pushstring(L, f->header.name ? f->header.name : "");        lua_setfield(L, -2, "name");
    lua_pushstring(L, f->header.field_name ? f->header.field_name : ""); lua_setfield(L, -2, "field_name");
    lua_pushstring(L, _introspection_type_name(f->header.type));    lua_setfield(L, -2, "type");
    lua_pushstring(L, f->header.type_name ? f->header.type_name : ""); lua_setfield(L, -2, "type_name");
    lua_pushinteger(L, (lua_Integer)f->header.offset);              lua_setfield(L, -2, "offset");
    lua_pushinteger(L, (lua_Integer)f->header.size);                lua_setfield(L, -2, "size");
    if(f->header.type == DT_INTROSPECTION_TYPE_ARRAY)
    {
      lua_pushinteger(L, (lua_Integer)f->Array.count);              lua_setfield(L, -2, "array_count");
      lua_pushstring(L, _introspection_type_name(f->Array.type));   lua_setfield(L, -2, "array_elem_type");
    }
    lua_rawseti(L, -2, i++);
  }
  lua_setfield(L, -2, "linear");

  // the top-level children (root struct), i.e. what get_params iterates.
  lua_newtable(L);
  dt_introspection_field_t *root = root_struct(module);
  int j = 1;
  if(root)
    for(dt_introspection_field_t **it = root->Struct.fields; it && *it; it++)
    {
      lua_pushstring(L, (*it)->header.name ? (*it)->header.name : "");
      lua_rawseti(L, -2, j++);
    }
  lua_setfield(L, -2, "top_level");
  return 1;
}

/* darktable.develop.enable_module(op, instance, enabled)
   ==========================================================================
   Toggle a module on/off, commit it as a history item, and (re)build the pipe
   so a subsequent preview() DETERMINISTICALLY reflects the new enabled state.
   Runs on the GUI thread (gtk_wrap), same threading contract as set_params.

   MIRRORS THE GUI ENABLE-TOGGLE (develop/imageop.c:1097 _gui_off_callback):
   the header on/off button sets `module->enabled` itself and then calls
   dt_dev_add_history_item(dev, module, FALSE) -- it passes FALSE because it has
   already flipped the flag; the enable arg would only force-enable. We do the
   same: set module->enabled explicitly (covers BOTH directions -- the enable
   arg of add_history_item never disables) and then commit.

   We use dt_dev_add_NEW_history_item (not the plain one the GUI uses) on
   purpose: the GUI relies on a focus_hash change to force a distinct undo entry
   between toggles, but a headless driver never focuses the widget, so a plain
   commit would COALESCE the toggle into the top item (same op, identical
   params -> develop.c "same operation, change params" branch, history_end
   unchanged). Forcing a new item makes each toggle its own undo step and makes
   history grow deterministically (acceptance T1.4-S1/S2). new_item also flags
   the pipes DT_DEV_PIPE_SYNCH (a full param re-commit), not merely TOP_CHANGED.

   THE RACE FIX (the primary objective of T1.4) -----------------------------
   T1.7 observed that enabling a module while a preview render was already in
   flight left the module rendering as a NO-OP ~50% of sessions, persistently.
   ROOT CAUSE (verified in source): the pixelpipe CACHE hash
   (develop/pixelpipe_cache.c:137) decides whether an upstream piece
   contributes to a downstream cache key with
       const gboolean included = piece->module->enabled || piece->enabled;
   i.e. it reads the LIVE, GUI-thread `module->enabled` LOCK-FREE from the
   worker thread. Sequence that poisons the cache:
     1. set_params() on the still-DISABLED module commits its params and kicks a
        preview render W1. W1 synched piece->enabled = FALSE, so it SKIPS the
        module (_skip_piece_on_tags, pixelpipe_hb.c:1785) and renders the
        module OFF.
     2. enable_module() flips module->enabled = TRUE on the GUI thread WHILE W1
        is still running.
     3. As W1 computes the DOWNSTREAM output cache hash it now reads
        module->enabled == TRUE, so it INCLUDES the module's piece->hash in the
        key (H_on) -- but the buffer it actually produced has the module OFF.
        W1 therefore stores an OFF buffer under the ON hash: a poisoned line.
     4. The next render targets H_on, HITS W1's poisoned line, and serves the
        OFF buffer -> the enable is a visible no-op. The line survives, so the
        whole session stays wrong (~50%, depending on whether the flip lands
        inside W1's hash window).
   The old enable path (plain dt_dev_add_history_item) only set TOP_CHANGED +
   invalidated buffers; it never FLUSHED the pipe cache, so the poison was
   served.
   FIX: after committing, call dt_dev_pixelpipe_rebuild(dev) -- the same
   canonical "structure changed" call the GUI multi-instance path uses
   (imageop.c:786). It sets DT_DEV_PIPE_REMOVE + cache_obsolete=TRUE +
   dt_dev_invalidate_all (dev->timestamp++) on all pipes. On the next worker
   pass this (a) takes pipe->busy_mutex, which BLOCKS until any in-flight render
   has fully finished writing its cache (the recursive process runs under
   busy_mutex, pixelpipe_hb.c:3083), (b) rebuilds every node so
   piece->enabled = module->enabled is consistent by construction, and (c)
   FLUSHES the cache (pixelpipe_hb.c:3173), discarding any poisoned line. After
   the flip module->enabled stays TRUE, so no future render can reproduce the
   piece/module disagreement. The timestamp bump means preview()'s freshness
   gate (status==VALID && input_timestamp>=want_ts, captured AFTER this call)
   can only be satisfied by a render that started after the rebuild -- it can
   never accept the pre-enable frame. Deterministic: enable -> preview reflects
   the module 10/10.

   Returns {ok=true, op, instance, enabled=<bool>} or {error="..."}. */
static int enable_module_cb(lua_State *L)
{
  const char *op = luaL_checkstring(L, 1);
  const int instance = luaL_optinteger(L, 2, 0);
  const gboolean want = lua_toboolean(L, 3);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  dt_iop_module_t *module = _find_module(dev, op, instance);
  if(!module)
    return push_error(L, "no module '%s' instance %d in current darkroom", op, instance);

  // 1. flip the live flag (mirrors _gui_off_callback), then commit a distinct
  //    history entry. Passing `want` also refreshes the GUI on/off button icon
  //    for the enable direction (add_history_item_ext's `if(enable)` block).
  module->enabled = want;
  dt_dev_add_new_history_item(dev, module, want);

  // 2. RACE FIX: rebuild topology + flush the pixelpipe cache (see note above).
  dt_dev_pixelpipe_rebuild(dev);

  // 3. GUI WIDGET REFRESH (FIX 1): re-sync the on/off button + all module
  //    widgets so the panel reflects the new enabled state immediately. Same
  //    reasoning/guard as set_params (on the GUI thread, no history re-entry).
  if(module->expander) dt_iop_gui_update(module);

  lua_newtable(L);
  lua_pushboolean(L, TRUE);            lua_setfield(L, -2, "ok");
  lua_pushstring(L, module->op);       lua_setfield(L, -2, "op");
  lua_pushinteger(L, module->multi_priority); lua_setfield(L, -2, "instance");
  lua_pushboolean(L, module->enabled); lua_setfield(L, -2, "enabled");
  return 1;
}

/* darktable.develop.add_instance(op)
   ==========================================================================
   Create a new multi-instance of module `op`, mirroring the GUI "new instance"
   path (develop/imageop.c:703 dt_iop_gui_duplicate, reached from the module
   header's multi-instance menu). Runs on the GUI thread (gtk_wrap), so calling
   the GUI helper directly is safe and gives us, in one call: the base module
   pushed to history, dt_dev_module_duplicate() (develop.c:3720 -- clones params
   + assigns a fresh unique multi_priority + inserts into dev->iop and the
   iop-order list), gui_init/expander setup, the new instance committed to
   history, and dt_dev_pixelpipe_rebuild() to wire the new node into all pipes.

   The new instance therefore immediately shows up in active_modules() (walks
   dev->iop) and is targetable by op + its multi_priority; set_params on it
   writes only ITS params blob, independent of instance 0.

   We resolve the base as instance 0 (the base multi_priority), falling back to
   the lowest-priority instance present. Modules flagged IOP_FLAGS_ONE_INSTANCE
   forbid duplicates (same gate the GUI uses, imageop.c:971) -> graceful error,
   never a crash (acceptance T1.4-S4).

   Returns {ok=true, op, instance=<new multi_priority>, base_instance,
   multi_name} or {error="..."}. */
static int add_instance_cb(lua_State *L)
{
  const char *op = luaL_checkstring(L, 1);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  // Prefer the base instance (multi_priority 0); else the lowest-priority one.
  dt_iop_module_t *base = _find_module(dev, op, 0);
  if(!base)
  {
    for(GList *it = dev->iop; it; it = g_list_next(it))
    {
      dt_iop_module_t *m = (dt_iop_module_t *)it->data;
      if(m && !g_strcmp0(m->op, op) && (!base || m->multi_priority < base->multi_priority))
        base = m;
    }
  }
  if(!base)
    return push_error(L, "no module '%s' in current darkroom", op);

  if(base->flags() & IOP_FLAGS_ONE_INSTANCE)
    return push_error(L, "module '%s' does not support multiple instances", op);

  // mirror the GUI new-instance path verbatim (copy_params=TRUE seeds the clone
  // from the base; set_params later diverges it independently).
  dt_iop_module_t *inst = dt_iop_gui_duplicate(base, TRUE);
  if(!inst)
    return push_error(L, "failed to duplicate module '%s'", op);

  lua_newtable(L);
  lua_pushboolean(L, TRUE);                 lua_setfield(L, -2, "ok");
  lua_pushstring(L, inst->op);              lua_setfield(L, -2, "op");
  lua_pushinteger(L, inst->multi_priority); lua_setfield(L, -2, "instance");
  lua_pushinteger(L, base->multi_priority); lua_setfield(L, -2, "base_instance");
  lua_pushstring(L, inst->multi_name);      lua_setfield(L, -2, "multi_name");
  return 1;
}

/* darktable.develop.history_count()
   Read-only diagnostic: {count=<g_list_length(dev->history)>,
   history_end=<dev->history_end>}. Used to observe native history coalescing
   (PLAN.md §3.3) without scraping logs. Reads live develop state on the GUI
   thread (gtk_wrap); history length is only ever mutated on the GUI thread, so
   the read needs no extra lock. */
static int history_count_cb(lua_State *L)
{
  dt_develop_t *dev = darktable.develop;
  if(!dev)
    return push_error(L, "no darkroom image loaded");
  lua_newtable(L);
  lua_pushinteger(L, dev->history ? (lua_Integer)g_list_length(dev->history) : 0);
  lua_setfield(L, -2, "count");
  lua_pushinteger(L, dev->history_end);
  lua_setfield(L, -2, "history_end");
  return 1;
}

/* Human-readable name for a dt_dev_zoom_t (settings.h:27). */
static const char *_zoom_name(dt_dev_zoom_t z)
{
  switch(z)
  {
    case DT_ZOOM_FIT:          return "fit";
    case DT_ZOOM_FILL:         return "fill";
    case DT_ZOOM_1:            return "1:1";
    case DT_ZOOM_FREE:         return "free";
    case DT_ZOOM_POSITION:     return "position";
    case DT_ZOOM_MOVE:         return "move";
    case DT_ZOOM_SCROLL:       return "scroll";
    case DT_ZOOM_FULL_PREVIEW: return "full_preview";
    case DT_ZOOM_RESTORE:      return "restore";
    default:                   return "?";
  }
}

/* Push one viewport's zoom/pan descriptor as a table (FIX 3).
   Uses the canonical accessors the darkroom GUI uses:
     * dt_dev_get_viewport_params (develop.c:3446) reads zoom/closeup/zoom_x/
       zoom_y under global_mutex; x/y are returned NORMALIZED and CENTERED --
       0 = image center, roughly [-0.5,+0.5] toward each edge -- via the pipe's
       distort transform (only when port->pipe is set, else left at 0).
     * dt_dev_get_zoom_scale (develop.c:946) gives the effective on-screen pixel
       scale (device px per processed px) for the current zoom mode; computed
       only when port->pipe exists (it needs the processed size), else we fall
       back to the raw stored port->zoom_scale.
   Fields: {zoom=<int enum>, zoom_label, closeup, zoom_x, zoom_y, scale,
            zoom_scale_raw, viewport_width, viewport_height, region}.

   region (POST-FIX -- the drop-in coord) ----------------------------------
   The raw zoom_x/zoom_y above are CENTER-relative (image center = 0) and can be
   NEGATIVE, so they do NOT compose with get_preview()'s `region` arg, which is
   TOP-LEFT and must be in [0,1]. `region` fixes that: it is the sub-rectangle of
   the FULL image currently visible in this window, expressed as TOP-LEFT
   normalized 0..1 image coords {x,y,w,h}, CLAMPED to [0,1]. It is therefore
   directly valid as get_preview()'s `region` with no caller math.

   Derivation (mirrors the GUI's own visible-rect math, verified against source):
     * Visible extent as a fraction of the full image, per axis, is exactly the
       "box" that dt_dev_get_zoom_bounds (develop.c:2823) computes:
           w = port->width  / (procw * scale)
           h = port->height / (proch * scale)
       where procw/proch = processed image size (dt_dev_get_processed_size) and
       `scale` is the device-px-per-processed-px zoom scale (closeup's extra 2x
       is already folded into `scale` via the 1<<closeup factor for DT_ZOOM_1).
     * Visible CENTER in 0..1 image coords is {0.5 + zoom_x, 0.5 + zoom_y}
       (zoom_x/zoom_y are center-relative, image center -> 0). This matches the
       GUI expose transform (darkroom.c:1019): the drawn top-left in processed px
       is (-0.5*width/scale + 0.5*wd + zoom_x*wd), i.e. normalized
       x = 0.5 + zoom_x - w/2.  So top-left = center - half-extent:
           x = 0.5 + zoom_x - w/2,  y = 0.5 + zoom_y - h/2.
     * Clamp: at "fit" the box comes out >=1 per axis and collapses to {0,0,1,1};
       when zoomed/panned it is a sub-rectangle clamped so x>=0, y>=0,
       x+w<=1, y+h<=1 (never negative, never exceeding the image edge).
   Omitted (nil) when there is no processed size to derive it from (no pipe yet).
   Units documented above. Read-only. */
static void _push_viewport(lua_State *L, dt_dev_viewport_t *port)
{
  dt_dev_zoom_t zoom = DT_ZOOM_FIT;
  int closeup = 0;
  float zx = 0.0f, zy = 0.0f;
  dt_dev_get_viewport_params(port, &zoom, &closeup, &zx, &zy);

  float scale = port->zoom_scale; // raw fallback when no pipe to size against
  if(port->pipe)
    scale = dt_dev_get_zoom_scale(port, zoom, 1 << closeup, FALSE);

  lua_newtable(L);
  lua_pushinteger(L, (lua_Integer)zoom);      lua_setfield(L, -2, "zoom");
  lua_pushstring(L, _zoom_name(zoom));        lua_setfield(L, -2, "zoom_label");
  lua_pushinteger(L, closeup);                lua_setfield(L, -2, "closeup");
  lua_pushnumber(L, zx);                      lua_setfield(L, -2, "zoom_x");
  lua_pushnumber(L, zy);                      lua_setfield(L, -2, "zoom_y");
  lua_pushnumber(L, scale);                   lua_setfield(L, -2, "scale");
  lua_pushnumber(L, port->zoom_scale);        lua_setfield(L, -2, "zoom_scale_raw");
  lua_pushinteger(L, port->width);            lua_setfield(L, -2, "viewport_width");
  lua_pushinteger(L, port->height);           lua_setfield(L, -2, "viewport_height");

  // region: TOP-LEFT, clamped [0,1] -- drop-in for get_preview's `region`.
  int procw = 0, proch = 0;
  dt_dev_get_processed_size(port, &procw, &proch);
  if(procw > 0 && proch > 0 && scale > 0.0f)
  {
    // fraction of the full image visible per axis (dt_dev_get_zoom_bounds math)
    float w = (float)port->width  / ((float)procw * scale);
    float h = (float)port->height / ((float)proch * scale);
    if(w > 1.0f) w = 1.0f;
    if(h > 1.0f) h = 1.0f;
    // top-left = visible-center (0.5 + zoom) - half-extent
    float x = 0.5f + zx - 0.5f * w;
    float y = 0.5f + zy - 0.5f * h;
    // clamp so the whole rect stays inside [0,1]: x>=0, y>=0, x+w<=1, y+h<=1
    if(x > 1.0f - w) x = 1.0f - w;
    if(y > 1.0f - h) y = 1.0f - h;
    if(x < 0.0f) x = 0.0f;
    if(y < 0.0f) y = 0.0f;

    lua_newtable(L);
    lua_pushnumber(L, x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, w); lua_setfield(L, -2, "w");
    lua_pushnumber(L, h); lua_setfield(L, -2, "h");
    lua_setfield(L, -2, "region");
  }
}

/* darktable.develop.get_viewport()
   ==========================================================================
   Read-only snapshot of the darkroom CANVAS zoom/pan state so an agent can
   render "what the user is actually looking at". Runs on the GUI thread
   (gtk_wrap): the viewport fields are only ever mutated on the GUI thread and
   dt_dev_get_viewport_params locks global_mutex internally, so this read is
   consistent by construction.

   Reports BOTH viewports darktable maintains (develop.h:356 `full, preview2`):
     main     -- dev->full, the primary darkroom canvas (always present in
                 darkroom).
     preview2 -- dev->preview2, the SECOND (secondary/other-monitor) darkroom
                 window. It only carries a live zoom/pan state while that window
                 is OPEN. The open/active signal is dev->second_wnd /
                 dev->preview2.widget, both set when the window is created
                 (darkroom.c:5403/5426) and NULL'd when it is closed
                 (darkroom.c:5334-5335). When inactive we still return the
                 preview2 key with {active=false} and NO zoom fields, never an
                 error.

   Guard: not in darkroom (no dev / not gui_attached / no image) -> graceful
   {error=...}, never a crash (acceptance: error in lighttable).

   Return: {main={zoom,zoom_label,closeup,zoom_x,zoom_y,scale,...,region},
            preview2={active=<bool>[, ...same fields (incl. region) when active]}}.

   COMPOSING WITH get_preview: pass main.region (or preview2.region) STRAIGHT
   INTO get_preview()'s `region` arg -- it is TOP-LEFT and clamped to [0,1], so
   it is always valid (no >=0 / range error). Do NOT use the raw zoom_x/zoom_y
   for that: they are center-relative and can be negative. region == "the crop
   of the full image the user is currently looking at in this window". When a
   viewport has no processed pipe yet (or preview2 is inactive) there is no
   region key -- callers must treat it as full-frame / skip the crop. */
static int get_viewport_cb(lua_State *L)
{
  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->gui_attached || !dt_is_valid_imgid(dev->image_storage.id))
    return push_error(L, "not in darkroom / no image loaded");

  lua_newtable(L); // result

  // main canvas (dev->full) -- always present in darkroom.
  _push_viewport(L, &dev->full);
  lua_setfield(L, -2, "main");

  // second window (dev->preview2) -- only live while the window is open.
  const gboolean p2_active = (dev->second_wnd != NULL) && (dev->preview2.widget != NULL);
  if(p2_active)
  {
    _push_viewport(L, &dev->preview2);       // pushes the descriptor table
    lua_pushboolean(L, TRUE);
    lua_setfield(L, -2, "active");           // add active=true onto it
  }
  else
  {
    lua_newtable(L);
    lua_pushboolean(L, FALSE); lua_setfield(L, -2, "active");
  }
  lua_setfield(L, -2, "preview2");

  return 1;
}

/* ==========================================================================
   darktable.develop.preview(max_w, max_h)
   --------------------------------------------------------------------------
   Encode a PNG of the CURRENT LIVE darkroom edit straight from the preview
   pixelpipe's backbuffer -- WITHOUT writing anything to the DB/XMP. This is
   the piece pure Lua could not do (T0.3 proved the export path reads the
   COMMITTED DB history via a fresh dt_develop_t, not the live in-memory edit).
   Together with set_params it closes the live conversational loop.

   THREADING DESIGN (PLAN.md Risk #1 / §3.4 -- read before touching) ---------
   The preview pixelpipe processes on a control WORKER thread
   (dt_dev_process_preview -> dt_control_add_job_res(DT_CTL_WORKER_ZOOM_FILL),
   develop.c:290-294), NOT on the GUI thread, and NOT on the Lua thread. The
   worker fills pipe->backbuf under pipe->backbuf_mutex and flips pipe->status
   to DT_DEV_PIXELPIPE_VALID at the very end of dt_dev_process_image_job
   (develop.c:867, after the backbuf memcpy at pixelpipe_hb.c:3299). So:

     * We DELIBERATELY do NOT register preview() wrapped in dt_lua_gtk_wrap.
       Wrapping would marshal the WHOLE callback -- including the freshness
       wait -- onto the GUI main thread and block it. A GUI-thread busy-wait
       would starve the very main loop whose expose/redraw drives the worker
       and, on a single flow, could stall waiting for a completion that the
       blocked loop can no longer help deliver. Instead preview() runs on the
       Lua thread and the WAIT is a bounded sleep-poll on the Lua thread
       (dt_lua_unlock + g_usleep + dt_lua_lock, exactly like dt.control.sleep,
       call.c:603): sleeping the Lua thread releases the Lua lock and never
       touches the GUI main loop, so the GUI thread and the worker run freely
       to completion while we wait.

     * The ONE step that belongs on the GUI thread -- taking a consistent
       snapshot of dev + deciding whether to (re)trigger the worker -- is
       isolated in _run_on_gui_sync() (a minimal g_main_context_invoke_full +
       GCond mirror of gtk_wrap, call.c:648). It releases the Lua lock while
       blocked so the GUI thread can never be starved of it. The trampoline
       itself touches no Lua state, so there is no lock inversion.

     * The backbuf GRAB is done on the Lua thread directly under
       pipe->backbuf_mutex -- that mutex IS the documented cross-thread
       contract for backbuf (the GUI expose locks it in view.c:1944, the
       worker locks it when writing, pixelpipe_hb.c:3278, and cleanup locks it
       before freeing, pixelpipe_hb.c:389). We only ever hold backbuf_mutex,
       memcpy out, and release -- a single lock, no nesting, so no deadlock and
       no lock-ordering inversion. PNG encoding happens AFTER unlock, off-lock.

   FRESHNESS SIGNAL (the key anti-stale guard) -----------------------------
   set_params -> dt_dev_add_history_item -> dt_dev_invalidate_all
   (develop.c:314) sets preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY and does
   dev->timestamp++ synchronously on the GUI thread. Every worker run stamps
   pipe->input_timestamp = dev->timestamp at its START (develop.c:632) and only
   reaches VALID at its END. So a frame is FRESH for the current edit iff
     status == DT_DEV_PIXELPIPE_VALID && input_timestamp >= want_ts
   where want_ts is dev->timestamp captured after the edit. A leftover VALID
   from a pre-edit run has input_timestamp < want_ts and is correctly rejected
   -- this is what prevents returning a stale (old-edit) preview. The reserved
   job slot (jobs.c:353-385) guarantees our dt_dev_process_preview either
   replaces a still-queued job or is queued behind a running one, so a fresh
   run for want_ts is always scheduled even if a stale run is mid-flight.

   RETURN (PLAN.md §3.4):
     ok:        {status="ok", path=<png>, width=W, height=H}
     timeout:   {status="processing", stale_preview=<path or absent>}
     no image / not in darkroom / no backbuf yet: {error="..."}
   ========================================================================== */

/* Minimal "run this C function synchronously on the GUI main thread" helper,
   used only for the small snapshot+trigger step of preview(). Modeled on
   gtk_wrap (call.c:648): if already on the GUI thread, call inline; otherwise
   invoke on the main context and block until done, releasing the Lua lock
   across the wait so the GUI thread is never starved of it. */
typedef struct
{
  GMutex m;
  GCond c;
  gboolean done;
  void (*fn)(void *);
  void *data;
} dt_lua_gui_call_t;

static gboolean _gui_call_trampoline(gpointer p)
{
  dt_lua_gui_call_t *g = (dt_lua_gui_call_t *)p;
  g->fn(g->data);
  g_mutex_lock(&g->m);
  g->done = TRUE;
  g_cond_signal(&g->c);
  g_mutex_unlock(&g->m);
  return G_SOURCE_REMOVE;
}

static void _run_on_gui_sync(void (*fn)(void *), void *data)
{
  if(pthread_equal(darktable.control->gui_thread, pthread_self()))
  {
    fn(data);
    return;
  }
  dt_lua_gui_call_t g = { .done = FALSE, .fn = fn, .data = data };
  g_mutex_init(&g.m);
  g_cond_init(&g.c);
  dt_lua_unlock();
  g_main_context_invoke_full(NULL, G_PRIORITY_HIGH_IDLE, _gui_call_trampoline, &g, NULL);
  g_mutex_lock(&g.m);
  while(!g.done) g_cond_wait(&g.c, &g.m);
  g_mutex_unlock(&g.m);
  dt_lua_lock();
  g_mutex_clear(&g.m);
  g_cond_clear(&g.c);
}

/* GUI-thread snapshot + conditional (re)trigger of the preview worker. */
typedef struct
{
  gboolean in_darkroom;      // out: dev in darkroom with an image loaded
  int want_ts;               // out: dev->timestamp we require the frame to match
  dt_dev_pixelpipe_t *pp;    // out: the preview pipe pointer (stable while in darkroom)
} preview_snapshot_t;

static void _preview_snapshot_gui(void *data)
{
  preview_snapshot_t *s = (preview_snapshot_t *)data;
  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->gui_attached || !dev->preview_pipe
     || !dt_is_valid_imgid(dev->image_storage.id))
  {
    s->in_darkroom = FALSE;
    return;
  }
  s->in_darkroom = TRUE;
  s->pp = dev->preview_pipe;
  s->want_ts = (int)dev->timestamp;
  // Only kick a fresh worker if the current backbuf is not already a fresh
  // VALID frame for this edit; avoids a redundant re-render on a plain
  // (no preceding set_params) preview() call.
  if(!(s->pp->status == DT_DEV_PIXELPIPE_VALID && s->pp->input_timestamp >= s->want_ts))
    dt_dev_process_preview(dev);
}

/* Write an 8-bit RGB buffer (tightly packed, w*h*3) to a PNG file via libpng
   (the same lib darktable already links, CMakeLists.txt:457). Returns TRUE on
   success; on failure sets *err to a g_malloc'd message. */
static gboolean _write_png_rgb(const char *path, const uint8_t *rgb, int w, int h, char **err)
{
  FILE *f = g_fopen(path, "wb");
  if(!f) { if(err) *err = g_strdup_printf("cannot open '%s' for writing", path); return FALSE; }

  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if(!png) { fclose(f); if(err) *err = g_strdup("png_create_write_struct failed"); return FALSE; }
  png_infop info = png_create_info_struct(png);
  if(!info)
  {
    png_destroy_write_struct(&png, NULL);
    fclose(f);
    if(err) *err = g_strdup("png_create_info_struct failed");
    return FALSE;
  }
  if(setjmp(png_jmpbuf(png)))
  {
    png_destroy_write_struct(&png, &info);
    fclose(f);
    if(err) *err = g_strdup("libpng encode error");
    return FALSE;
  }
  png_init_io(png, f);
  png_set_compression_level(png, 6);
  png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8, PNG_COLOR_TYPE_RGB,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  for(int y = 0; y < h; y++)
    png_write_row(png, (png_const_bytep)(rgb + (size_t)y * (size_t)w * 3));
  png_write_end(png, info);
  png_destroy_write_struct(&png, &info);
  fclose(f);
  return TRUE;
}

/* Box-average downscale (or 1:1 copy) of a SUB-RECTANGLE of the backbuf into a
   packed 8-bit RGB buffer of size tw*th*3, converting darktable's on-screen
   pixel order to RGB.

   REGION (FIX 2): [rx, ry, rw, rh] is the source crop rectangle in backbuf
   pixel coords (rx,ry = top-left, rw x rh = size). The caller guarantees it is
   fully inside [0,bw) x [0,bh). Full-frame is just rx=ry=0, rw=bw, rh=bh. The
   rw x rh source area is box-downscaled to tw x th; source spans are computed
   RELATIVE to the region and offset by (rx,ry) into the backbuf.

   PIXEL ORDER: backbuf is wrapped verbatim by dt_view_create_surface() as
   CAIRO_FORMAT_RGB24 (view.c:2080-2088). On little-endian that 32-bit format
   stores each pixel as bytes [B, G, R, X] in memory. So per source pixel:
   B = p[0], G = p[1], R = p[2], byte 3 unused. `src_stride` is the backbuf's
   real row stride in bytes (backbuf_size / height == 4*width, matching cairo's
   4-byte-aligned RGB24 stride). Returns a g_malloc'd RGB buffer (caller frees). */
static uint8_t *_backbuf_to_rgb(const uint8_t *bb, int rx, int ry, int rw, int rh,
                                size_t src_stride, int tw, int th)
{
  uint8_t *out = g_malloc((size_t)tw * (size_t)th * 3);
  if(!out) return NULL;
  for(int ty = 0; ty < th; ty++)
  {
    // source row span [sy0, sy1) WITHIN the region, then offset by ry.
    const int sy0 = ry + (int)((int64_t)ty * rh / th);
    int sy1 = ry + (int)((int64_t)(ty + 1) * rh / th);
    if(sy1 <= sy0) sy1 = sy0 + 1;
    if(sy1 > ry + rh) sy1 = ry + rh;
    for(int tx = 0; tx < tw; tx++)
    {
      const int sx0 = rx + (int)((int64_t)tx * rw / tw);
      int sx1 = rx + (int)((int64_t)(tx + 1) * rw / tw);
      if(sx1 <= sx0) sx1 = sx0 + 1;
      if(sx1 > rx + rw) sx1 = rx + rw;

      uint64_t rs = 0, gs = 0, bs = 0, n = 0;
      for(int sy = sy0; sy < sy1; sy++)
      {
        const uint8_t *row = bb + (size_t)sy * src_stride;
        for(int sx = sx0; sx < sx1; sx++)
        {
          const uint8_t *px = row + (size_t)sx * 4; // 4 bytes/px, BGRX
          bs += px[0];
          gs += px[1];
          rs += px[2];
          n++;
        }
      }
      if(!n) n = 1;
      uint8_t *o = out + ((size_t)ty * tw + tx) * 3;
      o[0] = (uint8_t)(rs / n);
      o[1] = (uint8_t)(gs / n);
      o[2] = (uint8_t)(bs / n);
    }
  }
  return out;
}

/* Compute the preview cache directory ($XDG_CACHE_HOME/darktable-mcp/
   develop-previews via g_get_user_cache_dir, which honors XDG_CACHE_HOME ->
   /run/cache-mcp inside the bridge container), creating it if needed. Returns
   a g_malloc'd path, or NULL on failure. */
static char *_preview_cache_dir(void)
{
  char *dir = g_build_filename(g_get_user_cache_dir(), "darktable-mcp", "develop-previews", NULL);
  if(g_mkdir_with_parents(dir, 0700) != 0)
  {
    g_free(dir);
    return NULL;
  }
  return dir;
}

/* Encode a captured backbuf copy to a fresh PNG in the cache dir. On success
   pushes a g_malloc'd container path into out_path and sets out_w / out_h to
   the encoded dimensions; returns TRUE. On failure sets *err (g_malloc'd). */
static gboolean _encode_backbuf(const uint8_t *bb, int bw, int bh, size_t bufsize,
                                int rx, int ry, int rw, int rh,
                                int max_w, int max_h,
                                char **out_path, int *out_w, int *out_h, char **err)
{
  if(bh <= 0 || bw <= 0) { if(err) *err = g_strdup("empty backbuf"); return FALSE; }
  const size_t src_stride = bufsize / (size_t)bh; // == 4*bw for the RGB24 backbuf

  // Region (FIX 2) in backbuf pixels; caller clamps to the backbuf. rw x rh is
  // the source area we downscale from (full frame = 0,0,bw,bh). Aspect + the
  // no-upscale rule are applied to the CROP dims, so a small region can render
  // at up to its own 1:1 backbuf resolution (real detail) within max_w/max_h.
  if(rw <= 0 || rh <= 0) { if(err) *err = g_strdup("empty region"); return FALSE; }

  // target dims: preserve aspect, downscale only (never upscale past the crop).
  int tw = rw, th = rh;
  if(max_w > 0 || max_h > 0)
  {
    double sx = (max_w > 0) ? (double)max_w / (double)rw : DBL_MAX;
    double sy = (max_h > 0) ? (double)max_h / (double)rh : DBL_MAX;
    double s = MIN(sx, sy);
    if(s > 1.0) s = 1.0; // no upscale beyond the cropped region's resolution
    tw = (int)floor((double)rw * s);
    th = (int)floor((double)rh * s);
    if(tw < 1) tw = 1;
    if(th < 1) th = 1;
  }

  uint8_t *rgb = _backbuf_to_rgb(bb, rx, ry, rw, rh, src_stride, tw, th);
  if(!rgb) { if(err) *err = g_strdup("out of memory building rgb buffer"); return FALSE; }

  char *dir = _preview_cache_dir();
  if(!dir) { g_free(rgb); if(err) *err = g_strdup("cannot create preview cache dir"); return FALSE; }
  char *fname = g_strdup_printf("preview-%" G_GINT64_FORMAT ".png", g_get_real_time());
  char *path = g_build_filename(dir, fname, NULL);
  g_free(dir);
  g_free(fname);

  const gboolean ok = _write_png_rgb(path, rgb, tw, th, err);
  g_free(rgb);
  if(!ok) { g_free(path); return FALSE; }

  *out_path = path;
  *out_w = tw;
  *out_h = th;
  return TRUE;
}

static int preview_cb(lua_State *L)
{
  const int max_w = luaL_optinteger(L, 1, 0);
  const int max_h = luaL_optinteger(L, 2, 0);

  // REGION (FIX 2): optional normalized sub-rectangle preview(max_w,max_h,x,y,w,h)
  // with x,y,w,h in 0..1 of the VISIBLE (preview_pipe) frame -- x,y = top-left,
  // w,h = size. Coordinates are relative to the downscaled full frame the pipe
  // produced (backbuf_width/height), NOT the raw sensor image. When omitted we
  // keep full-frame behavior. Validate+clamp the NORMALIZED region here (fast,
  // before the render wait); it is mapped to backbuf pixels after the grab, once
  // bw/bh are known. Degenerate (w<=0 / h<=0) -> graceful error, never a crash.
  gboolean has_region = FALSE;
  double nx = 0.0, ny = 0.0, nw = 1.0, nh = 1.0;
  if(!lua_isnoneornil(L, 3) || !lua_isnoneornil(L, 4)
     || !lua_isnoneornil(L, 5) || !lua_isnoneornil(L, 6))
  {
    if(!lua_isnumber(L, 3) || !lua_isnumber(L, 4)
       || !lua_isnumber(L, 5) || !lua_isnumber(L, 6))
      return push_error(L, "region requires four numbers: x, y, w, h in 0..1");
    nx = lua_tonumber(L, 3); ny = lua_tonumber(L, 4);
    nw = lua_tonumber(L, 5); nh = lua_tonumber(L, 6);
    if(!(nw > 0.0) || !(nh > 0.0))
      return push_error(L, "region w and h must be > 0 (got w=%g h=%g)", nw, nh);
    // clamp origin to [0,1], then clamp size so the rect stays inside [0,1].
    if(nx < 0.0) nx = 0.0;
    if(nx > 1.0) nx = 1.0;
    if(ny < 0.0) ny = 0.0;
    if(ny > 1.0) ny = 1.0;
    if(nw > 1.0 - nx) nw = 1.0 - nx;
    if(nh > 1.0 - ny) nh = 1.0 - ny;
    has_region = TRUE;
  }

  // (a) GUI-thread snapshot + conditional (re)trigger of the preview worker.
  preview_snapshot_t snap = { .in_darkroom = FALSE, .want_ts = 0, .pp = NULL };
  _run_on_gui_sync(_preview_snapshot_gui, &snap);

  if(!snap.in_darkroom || !snap.pp)
    return push_error(L, "not in darkroom / no image loaded");

  dt_dev_pixelpipe_t *pp = snap.pp;
  const int want_ts = snap.want_ts;

  // (b) bounded sleep-poll on the LUA thread for a FRESH VALID frame (§3.4
  // timeout 2000 ms). Sleeping here releases the Lua lock and never touches the
  // GUI main loop, so the GUI thread + worker run freely to completion.
  const int timeout_ms = 2000;
  const int step_ms = 15;
  int waited = 0;
  gboolean fresh = FALSE;
  while(waited < timeout_ms)
  {
    if(pp->status == DT_DEV_PIXELPIPE_VALID && pp->input_timestamp >= want_ts)
    {
      fresh = TRUE;
      break;
    }
    dt_lua_unlock();
    g_usleep((gulong)step_ms * 1000);
    dt_lua_lock();
    waited += step_ms;
  }

  // (c) grab the backbuf under backbuf_mutex (the cross-thread contract), copy
  // it out, release, then encode off-lock. We grab whatever VALID frame exists
  // even on timeout so a stale (last-good) frame can be returned.
  dt_pthread_mutex_lock(&pp->backbuf_mutex);
  const uint8_t *bb = pp->backbuf;
  const int bw = pp->backbuf_width;
  const int bh = pp->backbuf_height;
  const size_t bufsize = pp->backbuf_size;
  uint8_t *copy = NULL;
  if(bb && bw > 0 && bh > 0 && bufsize >= (size_t)4 * (size_t)bw * (size_t)bh)
  {
    copy = g_malloc(bufsize);
    if(copy) memcpy(copy, bb, bufsize);
  }
  dt_pthread_mutex_unlock(&pp->backbuf_mutex);

  if(!copy)
  {
    // Nothing ever rendered: distinguish from the timeout-with-stale case.
    if(!fresh)
      return push_error(L, "no preview backbuf yet (pipe not ready after %d ms)", timeout_ms);
    return push_error(L, "preview pipe VALID but backbuf empty");
  }

  // Map the NORMALIZED region (relative to the visible backbuf frame) to backbuf
  // pixel coords. Full frame when no region was given. Round origin down / size
  // to nearest, then clamp hard inside [0,bw) x [0,bh) so a boundary region can
  // never read past the buffer; guarantee at least a 1px crop.
  int rx = 0, ry = 0, rw = bw, rh = bh;
  if(has_region)
  {
    rx = (int)floor(nx * bw);
    ry = (int)floor(ny * bh);
    rw = (int)llround(nw * bw);
    rh = (int)llround(nh * bh);
    if(rx < 0) rx = 0;
    if(rx > bw - 1) rx = bw - 1;
    if(ry < 0) ry = 0;
    if(ry > bh - 1) ry = bh - 1;
    if(rw < 1) rw = 1;
    if(rx + rw > bw) rw = bw - rx;
    if(rh < 1) rh = 1;
    if(ry + rh > bh) rh = bh - ry;
  }

  char *path = NULL;
  int outw = 0, outh = 0, encret;
  char *encerr = NULL;
  encret = _encode_backbuf(copy, bw, bh, bufsize, rx, ry, rw, rh, max_w, max_h,
                           &path, &outw, &outh, &encerr);
  g_free(copy);

  if(!encret)
  {
    char *m = encerr ? encerr : g_strdup("png encode failed");
    push_error(L, "%s", m);
    g_free(m);
    return 1;
  }

  lua_newtable(L);
  if(fresh)
  {
    // success: fresh frame of the current live edit.
    lua_pushstring(L, "ok");   lua_setfield(L, -2, "status");
    lua_pushstring(L, path);   lua_setfield(L, -2, "path");
    lua_pushinteger(L, outw);  lua_setfield(L, -2, "width");
    lua_pushinteger(L, outh);  lua_setfield(L, -2, "height");
  }
  else
  {
    // timed out waiting for the fresh pipe: return the last good frame.
    lua_pushstring(L, "processing");   lua_setfield(L, -2, "status");
    lua_pushstring(L, path);           lua_setfield(L, -2, "stale_preview");
    lua_pushinteger(L, outw);          lua_setfield(L, -2, "width");
    lua_pushinteger(L, outh);          lua_setfield(L, -2, "height");
  }
  // Echo the frame + (when cropped) the region actually rendered, in both
  // normalized and backbuf-pixel terms, so a caller can verify the crop.
  lua_pushinteger(L, bw); lua_setfield(L, -2, "frame_width");
  lua_pushinteger(L, bh); lua_setfield(L, -2, "frame_height");
  if(has_region)
  {
    lua_newtable(L);
    lua_pushnumber(L, nx); lua_setfield(L, -2, "x");
    lua_pushnumber(L, ny); lua_setfield(L, -2, "y");
    lua_pushnumber(L, nw); lua_setfield(L, -2, "w");
    lua_pushnumber(L, nh); lua_setfield(L, -2, "h");
    lua_pushinteger(L, rx); lua_setfield(L, -2, "px_x");
    lua_pushinteger(L, ry); lua_setfield(L, -2, "px_y");
    lua_pushinteger(L, rw); lua_setfield(L, -2, "px_w");
    lua_pushinteger(L, rh); lua_setfield(L, -2, "px_h");
    lua_setfield(L, -2, "region");
  }
  g_free(path);
  return 1;
}

/* darktable.develop.add_path_mask(op, instance, points, opacity[, feather])
   ==========================================================================
   Build a DRAWN PATH mask (DT_MASKS_PATH) from a normalized polygon and attach
   it to the module's blend group so the module's effect is RESTRICTED to that
   region. This is the C core of Phase 2 (object masks). Runs on the GUI thread
   (registered wrapped in dt_lua_gtk_wrap): it mutates dev->forms, the module's
   blend group, blend_params and the history stack, all GUI-thread state, and
   commits via dt_dev_add_masks_history_item / dt_dev_add_history_item, whose
   locking contract (see set_params_cb) requires the GUI thread.

   ARGS
     op        module operation name (e.g. "exposure").
     instance  multi_priority (PUBLIC instance key, see INSTANCE-KEY CONVENTION).
     points    Lua array of {x=..,y=..}, normalized 0..1 image coords, in polygon
               order. Requires >= 3 points; each is clamped to [0,1]. A missing
               table / non-numeric x or y is MALFORMED -> graceful {error}, never
               a crash (acceptance #4).
     opacity   0..1 mask opacity (default 1.0, clamped).
     feather   optional border half-width in normalized coords (default 0.005,
               clamped [0,0.5]); floored at 0.0005 like the GUI path (path.c:3046).

   NODE BUILD (mirrors path.c:3035-3070, but for a POLYGON) -----------------
   Each node is a dt_masks_point_path_t with corner = {x,y}. The GUI's freehand
   creation stores ctrl1/ctrl2 = -1 as an "auto bezier" sentinel that a LATER
   call to the static _path_init_ctrl_points() resolves via catmull-rom. But the
   mask evaluation path used by blending -- _path_get_pts_border() (path.c:1413,
   reached through dt_masks_get_points_border) -- does NOT call that resolver; it
   consumes ctrl1/ctrl2 verbatim. So injecting -1 sentinels would feed -1 as a
   literal control coordinate and produce garbage. We therefore write EXPLICIT
   control points: ctrl1 = ctrl2 = corner with state = DT_MASKS_POINT_STATE_USER
   (a "sharp" node, path.c:3076). Every bezier segment then degenerates to a
   straight edge between successive corners -> a faithful polygon, self-contained
   with no dependency on the static resolver. border[0]=border[1]=feather.

   WIRING (what save_creation does vs what we set) --------------------------
   dt_masks_gui_form_save_creation(dev, module, form, NULL) (masks.c:370):
     * names the form, appends it to dev->forms, calls dt_dev_add_masks_history;
     * if the module has no mask group yet, _group_create() (masks.c:307) makes
       one AND sets module->blend_params->mask_id = grp->formid for us;
     * appends a dt_masks_point_group_t {formid, state=SHOW|USE, opacity=conf}
       to the group and adds a second masks-history item.
   It does NOT touch blend_params->mask_mode. So AFTER save_creation we:
     (a) override the group point's opacity to the requested value (save_creation
         seeds it from the "plugins/darkroom/masks/opacity" conf) and re-add a
         masks-history item so the change is captured;
     (b) set blend_params->mask_mode |= DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK
         -- BOTH bits, exactly like the GUI drawn-mask toggle
         (blend_gui.c:1507); DEVELOP_MASK_MASK (1<<1) alone does not enable
         blending, DEVELOP_MASK_ENABLED (1) is the master gate;
     (c) commit blend_params + trigger a live reprocess via
         dt_dev_add_history_item(dev, module, module->enabled) -- the same call
         the GUI mode combo ends with (blend_gui.c end of
         _blendop_masks_mode_callback). This invalidates the pipe so the next
         preview() reflects the localized effect.
   Finally dt_iop_gui_update(module) refreshes the module's blend UI (consistent
   with set_params/enable_module).

   Returns {ok=true, formid, mask_id, points=<n>, opacity, feather} or {error}. */
static int add_path_mask_cb(lua_State *L)
{
  const char *op = luaL_checkstring(L, 1);
  const int instance = luaL_optinteger(L, 2, 0);
  luaL_checktype(L, 3, LUA_TTABLE);
  const double opacity_arg = luaL_optnumber(L, 4, 1.0);
  const double feather_arg = luaL_optnumber(L, 5, 0.005);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  dt_iop_module_t *module = _find_module(dev, op, instance);
  if(!module)
    return push_error(L, "no module '%s' instance %d in current darkroom", op, instance);

  if(!module->blend_params || !module->flags || !(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING))
    return push_error(L, "module '%s' does not support blending/masks", op);

  const int npoints = (int)lua_rawlen(L, 3);
  if(npoints < 3)
    return push_error(L, "path mask needs at least 3 points (got %d)", npoints);

  double opacity = opacity_arg;
  if(opacity < 0.0) opacity = 0.0;
  if(opacity > 1.0) opacity = 1.0;
  double feather = feather_arg;
  if(feather < 0.0) feather = 0.0;
  if(feather > 0.5) feather = 0.5;
  const float border = MAX(0.0005f, (float)feather);

  // Build the polygon form. On ANY malformed node, free the partial form and
  // report a graceful error (never crash, acceptance #4).
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_PATH);
  if(!form)
    return push_error(L, "dt_masks_create failed");

  for(int i = 1; i <= npoints; i++)
  {
    lua_rawgeti(L, 3, i); // point table
    if(lua_type(L, -1) != LUA_TTABLE)
    {
      lua_pop(L, 1);
      dt_masks_free_form(form);
      return push_error(L, "point %d is not a table {x=..,y=..}", i);
    }
    lua_getfield(L, -1, "x");
    lua_getfield(L, -2, "y");
    if(!lua_isnumber(L, -2) || !lua_isnumber(L, -1))
    {
      lua_pop(L, 3);
      dt_masks_free_form(form);
      return push_error(L, "point %d has non-numeric x/y", i);
    }
    double x = lua_tonumber(L, -2);
    double y = lua_tonumber(L, -1);
    lua_pop(L, 3); // y, x, point table

    if(x < 0.0) x = 0.0;
    if(x > 1.0) x = 1.0;
    if(y < 0.0) y = 0.0;
    if(y > 1.0) y = 1.0;

    dt_masks_point_path_t *bzpt = malloc(sizeof(dt_masks_point_path_t));
    if(!bzpt)
    {
      dt_masks_free_form(form);
      return push_error(L, "out of memory building path node");
    }
    bzpt->corner[0] = (float)x;
    bzpt->corner[1] = (float)y;
    // explicit sharp corner: ctrl == corner -> straight edges (see NODE BUILD).
    bzpt->ctrl1[0] = bzpt->ctrl2[0] = (float)x;
    bzpt->ctrl1[1] = bzpt->ctrl2[1] = (float)y;
    bzpt->border[0] = bzpt->border[1] = border;
    bzpt->state = DT_MASKS_POINT_STATE_USER;
    form->points = g_list_append(form->points, bzpt);
  }

  // WIRE: name form, append to dev->forms, create/extend the module's mask
  // group (sets blend_params->mask_id), add group point, masks-history items.
  dt_masks_gui_form_save_creation(dev, module, form, NULL);

  const dt_mask_id_t formid = form->formid;
  const dt_mask_id_t mask_id = module->blend_params->mask_id;

  // (a) override the group point opacity for THIS form to the requested value.
  dt_masks_form_t *grp = dt_masks_get_from_id(dev, mask_id);
  if(grp)
  {
    for(GList *l = grp->points; l; l = g_list_next(l))
    {
      dt_masks_point_group_t *gp = (dt_masks_point_group_t *)l->data;
      if(gp && gp->formid == formid)
      {
        gp->opacity = (float)opacity;
        break;
      }
    }
    // capture the opacity change into masks history.
    dt_dev_add_masks_history_item(dev, module, TRUE);
  }

  // (b) turn on the drawn-mask blend mode (both bits, like the GUI toggle).
  module->blend_params->mask_mode |= DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK;

  // (c) commit blend_params + live reprocess (invalidates the pipe so the next
  // preview() shows the localized effect). module->enabled is passed through --
  // enabling the module is enable_module's / set_params' job, not ours.
  dt_dev_add_history_item(dev, module, module->enabled);

  // refresh the module's blend UI (consistent with set_params/enable_module).
  if(module->expander) dt_iop_gui_update(module);

  lua_newtable(L);
  lua_pushboolean(L, TRUE);                    lua_setfield(L, -2, "ok");
  lua_pushstring(L, module->op);               lua_setfield(L, -2, "op");
  lua_pushinteger(L, module->multi_priority);  lua_setfield(L, -2, "instance");
  lua_pushinteger(L, (lua_Integer)formid);     lua_setfield(L, -2, "formid");
  lua_pushinteger(L, (lua_Integer)mask_id);    lua_setfield(L, -2, "mask_id");
  lua_pushinteger(L, npoints);                 lua_setfield(L, -2, "points");
  lua_pushnumber(L, opacity);                  lua_setfield(L, -2, "opacity");
  lua_pushnumber(L, border);                   lua_setfield(L, -2, "feather");
  lua_pushinteger(L, (lua_Integer)module->blend_params->mask_mode);
  lua_setfield(L, -2, "mask_mode");
  return 1;
}

/* darktable.develop.set_raster_source(consumer_op, consumer_instance,
 *                                      source_op, source_instance[, opacity])
 * =========================================================================
 * SPIKE T3.1 (research go/no-go): wire a downstream module's blend to consume
 * a RASTER mask produced by an upstream source module. This is the ONE piece
 * the existing bindings could not do (add_path_mask only does DRAWN masks).
 *
 * It sets the consumer's blend_params raster fields and commits, exactly like
 * the GUI raster-mask picker: mask_mode |= ENABLED|RASTER, raster_mask_source =
 * <source op>, raster_mask_instance = <source multi_priority>, raster_mask_id =
 * BLEND_RASTER_ID. dt_dev_add_history_item -> dt_iop_commit_blend_params then
 * registers the consumer into source->raster_mask.source.users (imageop.c:1951),
 * which is what makes dt_iop_is_raster_mask_used() true so the source actually
 * emits its mask (e.g. iop/rasterfile.c:560).
 *
 * The source module must be EARLIER in the pixelpipe (lower iop_order) than the
 * consumer, else dt_dev_get_raster_mask() refuses it (pixelpipe_hb.c:3539).
 * Runs on the GUI thread (gtk_wrap), same contract as add_path_mask.
 *
 * Returns {ok, consumer, source, raster_mask_source, raster_mask_instance,
 * mask_mode, opacity} or {error}. */
static int set_raster_source_cb(lua_State *L)
{
  const char *cop = luaL_checkstring(L, 1);
  const int cinst = luaL_optinteger(L, 2, 0);
  const char *sop = luaL_checkstring(L, 3);
  const int sinst = luaL_optinteger(L, 4, 0);
  const double opacity_arg = luaL_optnumber(L, 5, 100.0);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  dt_iop_module_t *consumer = _find_module(dev, cop, cinst);
  if(!consumer)
    return push_error(L, "no consumer module '%s' instance %d", cop, cinst);
  dt_iop_module_t *source = _find_module(dev, sop, sinst);
  if(!source)
    return push_error(L, "no source module '%s' instance %d", sop, sinst);

  if(!consumer->blend_params || !consumer->flags
     || !(consumer->flags() & IOP_FLAGS_SUPPORTS_BLENDING))
    return push_error(L, "consumer '%s' does not support blending", cop);

  if(source->iop_order >= consumer->iop_order)
    return push_error(L,
        "source '%s' (iop_order %g) must be EARLIER than consumer '%s' (iop_order %g)",
        sop, source->iop_order, cop, consumer->iop_order);

  double opacity = opacity_arg;
  if(opacity < 0.0) opacity = 0.0;
  if(opacity > 100.0) opacity = 100.0;

  // wire the raster sink, exactly like the GUI raster picker.
  consumer->blend_params->mask_mode |= DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER;
  g_strlcpy(consumer->blend_params->raster_mask_source, source->op,
            sizeof(consumer->blend_params->raster_mask_source));
  consumer->blend_params->raster_mask_instance = source->multi_priority;
  consumer->blend_params->raster_mask_id = BLEND_RASTER_ID;
  consumer->blend_params->raster_mask_invert = FALSE;
  consumer->blend_params->opacity = (float)opacity;

  // commit blend_params + live reprocess (dt_iop_commit_blend_params runs
  // inside, registering the raster-mask user link).
  dt_dev_add_history_item(dev, consumer, consumer->enabled);
  if(consumer->expander) dt_iop_gui_update(consumer);

  lua_newtable(L);
  lua_pushboolean(L, TRUE);                       lua_setfield(L, -2, "ok");
  lua_pushstring(L, consumer->op);                lua_setfield(L, -2, "consumer");
  lua_pushinteger(L, consumer->multi_priority);   lua_setfield(L, -2, "consumer_instance");
  lua_pushstring(L, source->op);                  lua_setfield(L, -2, "source");
  lua_pushinteger(L, source->multi_priority);     lua_setfield(L, -2, "source_instance");
  lua_pushstring(L, consumer->blend_params->raster_mask_source);
  lua_setfield(L, -2, "raster_mask_source");
  lua_pushinteger(L, consumer->blend_params->raster_mask_instance);
  lua_setfield(L, -2, "raster_mask_instance");
  lua_pushinteger(L, (lua_Integer)consumer->blend_params->mask_mode);
  lua_setfield(L, -2, "mask_mode");
  lua_pushnumber(L, opacity);                     lua_setfield(L, -2, "opacity");
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

  // set_params(op, instance, {..}): writes live params + commits history via
  // dt_dev_add_history_item -> MUST run on the GUI thread (see the long
  // threading note on set_params_cb). gtk_wrap gives us exactly that.
  lua_pushcfunction(L, set_params_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "set_params");

  // dump_introspection(op, instance): read-only diagnostic (T1.7 investigation).
  lua_pushcfunction(L, dump_introspection_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "dump_introspection");

  // enable_module(op, instance, bool): toggle a module + commit history. Writes
  // live develop state via dt_dev_add_history_item -> MUST run on the GUI thread.
  lua_pushcfunction(L, enable_module_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "enable_module");

  // add_instance(op): duplicates a module via the GUI new-instance path
  // (dt_iop_gui_duplicate). Writes live develop/GUI state -> MUST run on the
  // GUI thread.
  lua_pushcfunction(L, add_instance_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "add_instance");

  // add_path_mask(op, instance, points, opacity[, feather]): builds a drawn
  // PATH mask from a polygon and wires it to the module's blend group. Mutates
  // dev->forms / blend group / blend_params / history -> MUST run on the GUI
  // thread (see the long note on add_path_mask_cb).
  lua_pushcfunction(L, add_path_mask_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "add_path_mask");

  // set_raster_source(consumer_op, consumer_instance, source_op, source_instance
  // [, opacity]): SPIKE T3.1 -- wire a downstream module's blend to consume an
  // upstream module's RASTER mask. Mutates blend_params + history -> GUI thread.
  lua_pushcfunction(L, set_raster_source_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "set_raster_source");

  // history_count(): read-only diagnostic for coalescing observation.
  lua_pushcfunction(L, history_count_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "history_count");

  // get_viewport(): read-only darkroom canvas zoom/pan (main + preview2). Reads
  // live viewport state (global_mutex internally) -> run on the GUI thread.
  lua_pushcfunction(L, get_viewport_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "get_viewport");

  // preview(max_w, max_h): grabs the live preview_pipe backbuf and encodes a
  // PNG, with NO DB write. Registered DELIBERATELY WITHOUT dt_lua_gtk_wrap:
  // the freshness wait must NOT run on (and block) the GUI main loop -- it
  // runs on the Lua thread as a bounded sleep-poll, and only the small
  // snapshot+trigger step is marshaled onto the GUI thread internally via
  // _run_on_gui_sync (see the long threading note on preview_cb above).
  lua_pushcfunction(L, preview_cb);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "preview");

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
