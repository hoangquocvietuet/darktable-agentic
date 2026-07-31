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
#include "iop/retouch.h"
#include "lua/call.h"
#include "lua/configuration.h"
#include "lua/lua.h"
#include "lua/types.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <limits.h>
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
// API 7 (bugfix batch): (1) adds current_image() -> {has_image,id,path,filename}
// so a caller (mask_raster) can resolve the file for the image the user opened
// BY HAND in the GUI, without having had to open it via open_darkroom itself.
// (2) add_path_mask() now defaults to SMOOTH boundaries: it computes Catmull-Rom
// bezier control points per node (new optional `smooth` arg, default true) so
// rounded subjects no longer get a faceted outline; and `feather` is now
// interpreted as a FRACTION of the mask's own bounding box (default 0.02), not a
// frame-constant, so a small/thin subject no longer "glows"/bleeds. Purely
// additive at the wire level; the feather semantic change is documented on the
// tool. All API-6 fields kept.
#define DT_LUA_DEVELOP_API 7
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

/* darktable.develop.get_conf_string(key) -- read-only passthrough to
   dt_conf_get_string() for an arbitrary core conf key (e.g.
   "plugins/darkroom/lut3d/def_path", the lut3d module's configured LUT root
   directory). dt_conf_get_string() is backed by its own mutex
   (darktable.conf->mutex, control/conf.c:_conf_get_var) so this needs no
   GUI-thread marshaling, same as version_cb above. Unknown keys resolve to
   the confgen default (possibly empty string), never an error -- matches
   dt_conf_get_string()'s own behavior. */
static int get_conf_string_cb(lua_State *L)
{
  const char *key = luaL_checkstring(L, 1);
  gchar *value = dt_conf_get_string(key);
  lua_pushstring(L, value ? value : "");
  g_free(value);
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

/* darktable.develop.add_instance(op[, fields])
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

   fields (optional table): initial param overrides applied to the NEW
   instance right after duplication, via set_params_cb's own introspection
   write path (lua_call'd directly below -- same field semantics/clamping/
   unknown-field reporting as a standalone set_params call). This exists
   because dt_iop_gui_duplicate's copy_params=TRUE (imageop.c:757-759) does a
   flat memcpy of the BASE instance's params over whatever reload_defaults()
   just computed for the new instance -- for exposure specifically, that means
   a second instance inherits compensate_exposure_bias/compensate_hilite_pres
   from instance 0 even though reload_defaults would have given the new
   instance its own (usually FALSE) values (see exposure.c's reload_defaults,
   compensate_hilite_pres = dt_iop_is_first_instance(...)). A caller building a
   local dodge/burn instance wants exposure=0/compensate_*=false from the very
   first history entry, not as a lagging second edit.

   This does NOT create a second history entry: dev->focus_hash is already
   TRUE in any live darkroom session (dt_iop_request_focus only flips it
   FALSE->TRUE once, on the very first module focus after a history reload --
   see _dev_add_history_item_ext's dedup condition, develop/develop.c:
   1258-1268), so by the time this fields-write runs, module == hist->module
   (same instance dt_iop_gui_duplicate just committed) and focus_hash is
   unchanged from that commit -> the dedup condition holds and this second
   dt_dev_add_history_item (inside set_params_cb) REUSES the just-created
   history item (memcpy over it, develop.c:1305-1306) instead of appending a
   new one. Same mechanism already relied on for consecutive same-module
   set_params calls (documented in CLAUDE.md's LUT tooling section).

   Returns {ok=true, op, instance=<new multi_priority>, base_instance,
   multi_name, fields_applied=<set_params result, if fields given>} or
   {error="..."}. */
static int add_instance_cb(lua_State *L)
{
  const char *op = luaL_checkstring(L, 1);
  const gboolean has_fields = !lua_isnoneornil(L, 2);
  if(has_fields) luaL_checktype(L, 2, LUA_TTABLE);

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

  if(has_fields)
  {
    // Re-invoke set_params_cb as a REAL Lua call (lua_call, not a direct C
    // call) so its own luaL_check/lua_next arg indices resolve against a
    // fresh call frame (op/instance/fields), exactly as if a caller had made
    // a separate set_params(op, instance, fields) call right after
    // add_instance -- see the long note above for why this collapses into
    // the SAME history entry rather than adding a second one.
    lua_pushcfunction(L, set_params_cb);
    lua_pushstring(L, inst->op);
    lua_pushinteger(L, inst->multi_priority);
    lua_pushvalue(L, 2); // the fields table (arg 2 of add_instance)
    lua_call(L, 3, 1);   // -> {ok, applied, clamped, unknown_fields} or {error=...}
    lua_setfield(L, -2, "fields_applied");
  }

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

  // processed_width/processed_height (ADDED for set_viewport, see develop.c):
  // the EXACT, UNCLAMPED denominator the region math above divides by. A
  // caller inverting the region formula (set_viewport: region -> zoom_x/
  // zoom_y/scale) needs these directly -- back-deriving them from the
  // (possibly clamped, when boxw/boxh > 1 i.e. zoomed out past fit) `region.w`/
  // `region.h` above would silently misfire in that clamped case. Plain
  // additive fields, no existing reader breaks.
  lua_pushinteger(L, procw); lua_setfield(L, -2, "processed_width");
  lua_pushinteger(L, proch); lua_setfield(L, -2, "processed_height");

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

/* darktable.develop.backtransform_point(x, y[, len1][, len2])
 * ===========================================================================
 * Convert a point NORMALIZED 0..1 against the CURRENT PROCESSED/display
 * frame (the same frame get_viewport()/get_preview() use -- i.e. the image
 * AFTER crop/rotate/orientation/lens-correction have run) into a point
 * normalized 0..1 against the PIPE-INPUT frame that dt_masks (circle/path/
 * brush/gradient/ellipse -- and therefore retouch_add_shape/
 * retouch_update_shape/add_path_mask) actually store coordinates in.
 *
 * These two frames are IDENTICAL only when no active module changes the
 * image's width/height/aspect between pipe input and pipe output --
 * ORIENTATION ALONE already breaks this for any portrait shot (EXIF
 * orientation swaps X/Y, see src/iop/flip.c's modify_roi_out), and
 * crop/rotate&perspective/lens-correction change it further. This is
 * exactly the conversion every masks-directory GUI drag-commit handler performs
 * before writing a mask point (e.g. circle.c's
 * _circle_events_button_released: multiply by dt_masks_get_image_size's
 * wd/ht, dt_dev_distort_backtransform, divide by iwidth/iheight) -- this
 * binding is that same conversion, callable headless for a point supplied
 * programmatically instead of a mouse event. Bug: a caller deriving a
 * "normalized 0..1" point from get_viewport()/get_preview() and writing it
 * DIRECTLY into retouch_add_shape/add_path_mask (skipping this step) will
 * silently place the shape on the wrong part of the image whenever
 * orientation/crop/rotate/lens-correction is active -- confirmed via a
 * portrait-photo bugreport (2026-07-25) where a neck target ended up on
 * the chest, even with the FULL frame (no cropped viewport sub-region)
 * requested, which ruled out a simple region-math bug.
 *
 * len1/len2 (optional): a LENGTH (not a position), e.g. a circle
 * radius/feather, in the same processed-frame-normalized units as x/y.
 * Converted by backtransforming a second point offset by len1/len2 along x
 * and measuring the resulting pipe-input-frame PIXEL distance, then
 * dividing by mindim(iwidth, iheight) -- matching circle.c's own radius
 * convention EXACTLY (circle.c:1139-1144: `radius_px = radius_normalized *
 * MIN(iwidth, iheight)`, a uniform/circular radius, not stretched by
 * aspect ratio). Locally correct for affine transforms (crop/rotate/
 * orientation) and a first-order approximation under lens-correction/
 * perspective -- darktable's own mask geometry has no better answer for
 * either (circle creation only ever scales an EXISTING radius by a
 * relative drag factor, never computes an absolute one from a screen
 * distance -- see circle.c:296,554).
 *
 * Returns {x, y[, len1][, len2]} or {error=...}. Runs on the GUI thread. */
static int backtransform_point_cb(lua_State *L)
{
  const double x_arg = luaL_checknumber(L, 1);
  const double y_arg = luaL_checknumber(L, 2);
  const gboolean has_len1 = !lua_isnoneornil(L, 3);
  const double len1_arg = luaL_optnumber(L, 3, 0.0);
  const gboolean has_len2 = !lua_isnoneornil(L, 4);
  const double len2_arg = luaL_optnumber(L, 4, 0.0);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  float wd, ht, iwidth, iheight;
  dt_masks_get_image_size(&wd, &ht, &iwidth, &iheight);
  if(wd <= 0.0f || ht <= 0.0f || iwidth <= 0.0f || iheight <= 0.0f)
    return push_error(L, "backtransform_point: no processed pipe yet");

  const float x = CLAMPS(x_arg, 0.0, 1.0);
  const float y = CLAMPS(y_arg, 0.0, 1.0);
  const float mindim = fminf(iwidth, iheight);

  float center[2] = { x * wd, y * ht };
  dt_dev_distort_backtransform(dev, center, 1);

  lua_newtable(L);
  lua_pushnumber(L, center[0] / iwidth);  lua_setfield(L, -2, "x");
  lua_pushnumber(L, center[1] / iheight); lua_setfield(L, -2, "y");

  if(has_len1)
  {
    float edge[2] = { (x + (float)len1_arg) * wd, y * ht };
    dt_dev_distort_backtransform(dev, edge, 1);
    const float dx_px = edge[0] - center[0];
    const float dy_px = edge[1] - center[1];
    lua_pushnumber(L, sqrtf(dx_px * dx_px + dy_px * dy_px) / mindim);
    lua_setfield(L, -2, "len1");
  }
  if(has_len2)
  {
    float edge[2] = { (x + (float)len2_arg) * wd, y * ht };
    dt_dev_distort_backtransform(dev, edge, 1);
    const float dx_px = edge[0] - center[0];
    const float dy_px = edge[1] - center[1];
    lua_pushnumber(L, sqrtf(dx_px * dx_px + dy_px * dy_px) / mindim);
    lua_setfield(L, -2, "len2");
  }
  return 1;
}

/* Convert one mask/pipe-input-frame normalized point into a processed/display-
 * frame normalized point -- the exact INVERSE direction of
 * backtransform_point_cb above. Shared by transform_point_cb and
 * retouch_list_shapes_cb (the latter reports BOTH frames so a caller reading
 * shapes back never has to guess which frame the numbers are in; that guess
 * was itself a bug source -- see the 2026-07-26 overlay-render work).
 * Caller supplies the dims already fetched via dt_masks_get_image_size(). */
static gboolean _mask_point_to_display(dt_develop_t *dev,
                                       const float wd, const float ht,
                                       const float iwidth, const float iheight,
                                       const double mx, const double my,
                                       double *dx, double *dy)
{
  float pt[2] = { (float)mx * iwidth, (float)my * iheight };
  if(!dt_dev_distort_transform(dev, pt, 1)) return FALSE;
  *dx = pt[0] / wd;
  *dy = pt[1] / ht;
  return TRUE;
}

/* Same direction for a LENGTH (radius/feather) rather than a position. Input
 * is in dt_masks' own radius convention (normalized against
 * mindim(iwidth, iheight) -- circle.c:1139-1144); output is normalized against
 * the display frame's WIDTH, which is the convention the viewport/overlay
 * Python side uses for radii (never height, never the w/h mean). Measured by
 * transforming a second point offset along x, i.e. locally correct for affine
 * transforms and first-order under lens-correction/perspective -- same
 * accuracy caveat as backtransform_point's len1/len2. */
static gboolean _mask_len_to_display(dt_develop_t *dev,
                                     const float wd, const float ht,
                                     const float iwidth, const float iheight,
                                     const double mx, const double my,
                                     const double mlen, double *dlen)
{
  const float mindim = fminf(iwidth, iheight);
  float pts[4] = { (float)mx * iwidth,                          (float)my * iheight,
                   (float)mx * iwidth + (float)mlen * mindim,   (float)my * iheight };
  if(!dt_dev_distort_transform(dev, pts, 2)) return FALSE;
  const float ddx = pts[2] - pts[0];
  const float ddy = pts[3] - pts[1];
  *dlen = sqrtf(ddx * ddx + ddy * ddy) / wd;
  (void)ht;
  return TRUE;
}

/* darktable.develop.transform_point(x, y[, len1][, len2])
 * ===========================================================================
 * The INVERSE of backtransform_point: a point normalized 0..1 against the
 * PIPE-INPUT/mask frame (the frame dt_masks -- and therefore
 * retouch_list_shapes/get_mask -- stores geometry in) converted into a point
 * normalized 0..1 against the CURRENT PROCESSED/display frame (the frame
 * get_viewport()/get_preview()/capture_viewport render in).
 *
 * WHY THIS EXISTS: backtransform_point closed the WRITE path (viewport
 * coordinates -> mask geometry). The READ path was still broken: an agent that
 * wrote a shape via retouch_add_shape_in_viewport and then read it back with
 * retouch_list_shapes got mask-frame numbers while looking at a display-frame
 * render, so the two never matched whenever orientation/crop/rotate/
 * lens-correction was active (portrait shots: ALWAYS). Verifying "did my shape
 * land where I aimed?" or drawing an overlay on top of a captured render both
 * need this direction.
 *
 * len1/len2 (optional): lengths (e.g. a circle radius/feather) in dt_masks'
 * mindim-normalized convention, returned normalized against the display
 * frame's WIDTH. See _mask_len_to_display.
 *
 * Returns {x, y[, len1][, len2]} or {error=...}. Runs on the GUI thread. */
static int transform_point_cb(lua_State *L)
{
  const double x_arg = luaL_checknumber(L, 1);
  const double y_arg = luaL_checknumber(L, 2);
  const gboolean has_len1 = !lua_isnoneornil(L, 3);
  const double len1_arg = luaL_optnumber(L, 3, 0.0);
  const gboolean has_len2 = !lua_isnoneornil(L, 4);
  const double len2_arg = luaL_optnumber(L, 4, 0.0);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  float wd, ht, iwidth, iheight;
  dt_masks_get_image_size(&wd, &ht, &iwidth, &iheight);
  if(wd <= 0.0f || ht <= 0.0f || iwidth <= 0.0f || iheight <= 0.0f)
    return push_error(L, "transform_point: no processed pipe yet");

  const double x = CLAMPS(x_arg, 0.0, 1.0);
  const double y = CLAMPS(y_arg, 0.0, 1.0);

  double dx = 0.0, dy = 0.0;
  if(!_mask_point_to_display(dev, wd, ht, iwidth, iheight, x, y, &dx, &dy))
    return push_error(L, "transform_point: distort_transform failed");

  lua_newtable(L);
  lua_pushnumber(L, dx); lua_setfield(L, -2, "x");
  lua_pushnumber(L, dy); lua_setfield(L, -2, "y");

  if(has_len1)
  {
    double dlen = 0.0;
    if(!_mask_len_to_display(dev, wd, ht, iwidth, iheight, x, y, len1_arg, &dlen))
      return push_error(L, "transform_point: distort_transform failed (len1)");
    lua_pushnumber(L, dlen); lua_setfield(L, -2, "len1");
  }
  if(has_len2)
  {
    double dlen = 0.0;
    if(!_mask_len_to_display(dev, wd, ht, iwidth, iheight, x, y, len2_arg, &dlen))
      return push_error(L, "transform_point: distort_transform failed (len2)");
    lua_pushnumber(L, dlen); lua_setfield(L, -2, "len2");
  }
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

/* ==========================================================================
   darktable.develop.set_viewport(viewport, zoom_x, zoom_y, scale
                                   [, wait_for_pipe][, timeout_ms])
   darktable.develop.restore_viewport(viewport, zoom, closeup, zoom_x, zoom_y,
                                       scale[, wait_for_pipe][, timeout_ms])
   ==========================================================================
   Writable counterpart to get_viewport() (2026-07-31 "set-viewport-design"
   task). An agent states an ABSOLUTE zoom_x/zoom_y/scale -- the exact fields
   get_viewport() reads back (center-relative pan, dt_dev_get_zoom_scale-
   equivalent scale). This binding does NOT do region math (aspect-expand,
   mode translation, min_render_px_across): that is darktable_mcp/server.py's
   job, which reads processed_width/processed_height (added to
   _push_viewport above specifically for this) plus the current region/
   scale from get_viewport() to invert the region formula documented above
   _push_viewport, then calls this binding with the resulting zoom_x/zoom_y/
   scale. Division of labor mirrors viewport_coords.py's affine transforms:
   C provides exact primitives, Python does policy.

   WRITE PRIMITIVE (read in full, not just its first ~40 lines, per the task
   note) -- dt_dev_zoom_move(port, zoom, scale, closeup, x, y, constrain),
   develop.c:3187:
     * DT_ZOOM_POSITION sets zoom_x=x; zoom_y=y directly (absolute, center-
       relative) and returns BEFORE the big else-chain that writes port->
       zoom/closeup/zoom_scale -- those three fields are left untouched by a
       POSITION call.
     * Every OTHER zoom value that ever actually gets STORED into port->zoom
       (only DT_ZOOM_FIT/FILL/DT_ZOOM_1/FREE survive to that assignment;
       SCROLL/MOVE/FULL_PREVIEW/RESTORE are transient operation-selectors
       that remap internally before the assignment) sets port->closeup/
       zoom_scale/zoom unconditionally at the bottom of the else-chain, from
       whatever `scale`/`closeup` were passed -- AS LONG AS `closeup` is not
       DT_ZOOM_1's sentinel -1/-2 (interactive-toggle triggers; never used
       here, we always pass a concrete closeup).
     * x<0 (we pass -1.0f) on the zoom-mode call skips the cursor-relative
       "zoom under the mouse" adjustment entirely, leaving pan untouched --
       the following POSITION call sets it explicitly regardless.
     * A POSITION call passed pan-space (small, often positive) x/y values
       ALSO reaches that same cursor-adjustment code path (it is gated only
       on x,y>=0, not on the zoom type) but it is a verified no-op there:
       cur_scale and new_scale are both computed from port->zoom/closeup,
       which a pure POSITION call never modifies, so cur_scale==new_scale
       and the adjustment term (mouse_off*(1/cur_scale - 1/new_scale)) is
       algebraically zero.
   Net effect: setting scale+position from one intent takes exactly TWO
   calls -- (1) a zoom-mode call (DT_ZOOM_FREE for set_viewport per
   requirement 5; the ORIGINAL previous.zoom for restore_viewport, to
   reproduce the exact zoom_label) with x=y=-1, then (2) a DT_ZOOM_POSITION
   call with the target zoom_x/zoom_y. This mirrors the live GUI itself: a
   scroll-wheel zoom and a drag-pan are already two separate, non-atomic
   dt_dev_zoom_move calls from two different GTK signal handlers -- there is
   no existing atomic set-both primitive to fall back to instead.

   SCALE CLAMP (requirement 6) -- darktable's own unconstrained free-zoom
   bound (_zoom_constraint_bounds(constrained=FALSE, ...), develop.c:2892,
   static/non-exported -- reproduced here, rather than exporting it, to keep
   the diff to this file's own new code): regardless of image-size category,
   the UNCONSTRAINED bounds always collapse to the same
   [tscalefloor, tscaletop] = [MIN(0.5*fitscale*ppd, 1.0), 16.0] (tscale
   units, i.e. already ppd-multiplied) -- divide by ppd for plain `scale`.
   This is the exact bound darktable's own pinch-zoom uses unconstrained;
   free zoom (requirement 5) is that same unconstrained mode.

   WAIT-FOR-PIPE (requirement 3) watches the PORT'S OWN pipe (dev->full.pipe
   / dev->preview2.pipe), NOT dev->preview_pipe: confirmed those are
   different dt_dev_pixelpipe_t instances -- dt_dev_process_preview_job_run
   (control/jobs/develop_jobs.c) processes dev->preview_pipe with port=NULL,
   entirely independent of any dt_dev_viewport_t's zoom state; only
   dt_dev_process_image_job_run passes port=&dev->full. Freshness signal is
   DELIBERATELY NOT preview()'s timestamp check (status==VALID &&
   input_timestamp>=want_ts): a pure zoom/pan change does NOT bump
   dev->timestamp (only dt_dev_invalidate/invalidate_all do, for content/
   history edits -- develop.c:304-320), so a pipe already VALID from BEFORE
   this call would satisfy that check immediately even though it has not
   re-rendered at the new zoom at all. Instead this polls the port->pipe->
   changed DT_DEV_PIPE_ZOOMED bit, which dt_dev_zoom_move sets synchronously
   (develop.c:3388) and which dt_dev_process_image_job only clears once it
   has re-run dt_dev_pixelpipe_process at the CURRENT (new) zoom/scale/
   position (develop.c:788,837-858) -- an accurate, zoom-specific freshness
   signal, not a content-edit one. The worker run is triggered the same way
   preview() triggers dev->preview_pipe's: dt_dev_process_image(dev) /
   dt_dev_process_preview2(dev), both queuing to a WORKER thread pool
   (dt_control_add_job_res), independent of the GUI main loop pumping --
   confirmed by reading develop.c:283-302, the exact analogue of
   dt_dev_process_preview(). The snapshot+apply+trigger step runs on the GUI
   thread via _run_on_gui_sync (dt_dev_zoom_move's own
   dt_control_queue_redraw_widget calls want that thread); the wait itself
   is a bounded Lua-thread sleep-poll exactly like preview_cb's, for the
   same starve-the-GUI-thread reason documented on preview_cb above.

   KNOWN, ACCEPTED, NARROW RACE (documented, not fixed -- same class of risk
   preview()'s own comment accepts for its job-queue "reserved slot"
   guarantee): if a pipe job for this exact port is ALREADY mid-flight
   (pipe->processing) the moment we call, our explicit trigger no-ops
   (mirrors dt_dev_process_image's own guard) and that in-flight job may
   have started before our zoom change and so will not reflect it. The poll
   loop re-issues the GUI-thread trigger once more, at the halfway point of
   the timeout, as a cheap defensive retry; a fully provable zero-race
   guarantee would need job-queue internals changes, out of scope here.

   RENDERABLE_PX (requirement 4) is intentionally NOT computed by this
   binding: it only reports the geometry it actually changed (previous/
   applied/clamped/pipe_ready). darktable_mcp/server.py computes
   achieved.region via a plain get_viewport() re-read after this call
   returns (truthful by construction, no duplicated math) and renderable_px
   via an ACTUAL probe call to the existing preview() binding at the
   achieved region with a generous max_w/max_h, reading back its real
   reported width/height. A probe, not a formula, because dev->preview_pipe
   -- which capture_viewport's pixel path always reads regardless of which
   viewport's region is used -- has a FIXED native resolution entirely
   decoupled from any dev->full/preview2 zoom (see the port=NULL note
   above): computing renderable_px from this binding's own zoom state would
   NOT predict what capture_viewport actually returns. */

typedef struct
{
  const char *viewport_name;          // in: "main" | "preview2"
  gboolean is_set;                    // in: TRUE = set_viewport (clamp + DT_ZOOM_FREE),
                                       //     FALSE = restore_viewport (no clamp, caller's own mode)
  dt_dev_zoom_t mode;                 // in: DT_ZOOM_FREE (set) or previous.zoom (restore)
  int mode_closeup;                   // in: 0 (set) or previous.closeup (restore)
  float requested_scale;              // in
  float target_zoom_x, target_zoom_y; // in

  gboolean ok;                        // out
  gboolean viewport_not_active;       // out: preview2 requested but not open
  dt_dev_viewport_t *port;            // out
  dt_dev_pixelpipe_t *pipe;           // out

  dt_dev_zoom_t prev_zoom;            // out
  int prev_closeup;                   // out
  float prev_zoom_x, prev_zoom_y;     // out
  float prev_scale;                   // out (effective, dt_dev_get_zoom_scale)

  float applied_scale;                // out (after clamp, when is_set)
  gboolean scale_clamped;             // out
  float scale_floor, scale_ceiling;   // out
} dt_lua_viewport_op_t;

/* GUI-thread step: resolve the port, snapshot "previous", clamp scale
   (set_viewport only -- restore_viewport passes an already-known-good
   previous.scale through unclamped, there is nothing to clamp when
   reproducing an exact prior state), apply the two dt_dev_zoom_move calls,
   and queue a fresh worker run for the port's own pipe. Mirrors
   _preview_snapshot_gui's role for preview_cb below. */
static void _viewport_op_apply_gui(void *data)
{
  dt_lua_viewport_op_t *s = (dt_lua_viewport_op_t *)data;
  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->gui_attached || !dt_is_valid_imgid(dev->image_storage.id))
  {
    s->ok = FALSE;
    return;
  }

  dt_dev_viewport_t *port = NULL;
  if(!g_strcmp0(s->viewport_name, "main"))
  {
    port = &dev->full;
  }
  else if(!g_strcmp0(s->viewport_name, "preview2"))
  {
    const gboolean p2_active = (dev->second_wnd != NULL) && (dev->preview2.widget != NULL);
    if(!p2_active)
    {
      s->ok = FALSE;
      s->viewport_not_active = TRUE;
      return;
    }
    port = &dev->preview2;
  }
  else
  {
    s->ok = FALSE; // caller already validated the string; defensive only
    return;
  }

  s->port = port;
  s->pipe = port->pipe;

  dt_dev_get_viewport_params(port, &s->prev_zoom, &s->prev_closeup, &s->prev_zoom_x, &s->prev_zoom_y);
  s->prev_scale = dt_dev_get_zoom_scale(port, s->prev_zoom, 1 << s->prev_closeup, FALSE);

  float scale = s->requested_scale;
  const int closeup = s->mode_closeup;
  const dt_dev_zoom_t mode = s->mode;

  if(s->is_set)
  {
    const float fitscale = dt_dev_get_zoom_scale(port, DT_ZOOM_FIT, 1, FALSE);
    const float ppd = (float)port->ppd > 0.0f ? (float)port->ppd : 1.0f;
    s->scale_floor = fminf(0.5f * fitscale, 1.0f / ppd);
    s->scale_ceiling = 16.0f / ppd;
    if(scale < s->scale_floor)
    {
      scale = s->scale_floor;
      s->scale_clamped = TRUE;
    }
    else if(scale > s->scale_ceiling)
    {
      scale = s->scale_ceiling;
      s->scale_clamped = TRUE;
    }
  }
  s->applied_scale = scale;

  // (1) zoom-mode call: sets port->closeup/zoom_scale/zoom. x=y=-1 skips the
  // cursor-relative adjustment so pan is untouched here -- call (2) sets it
  // explicitly regardless.
  dt_dev_zoom_move(port, mode, scale, closeup, -1.0f, -1.0f, FALSE);
  // (2) absolute pan.
  dt_dev_zoom_move(port, DT_ZOOM_POSITION, 0.0f, 0, s->target_zoom_x, s->target_zoom_y, FALSE);

  // Queue a fresh worker run for THIS port's own pipe (mirrors preview()'s
  // explicit dt_dev_process_preview trigger). A no-op if a job for this pipe
  // is already in flight -- see the accepted-race note above.
  if(port == &dev->full)
    dt_dev_process_image(dev);
  else
    dt_dev_process_preview2(dev);

  s->ok = TRUE;
}

/* _run_on_gui_sync trampolines for the wait loop's defensive re-trigger
   below -- kept as real functions (not a cast function pointer) to avoid
   any -Wcast-function-type risk under -Werror. */
static void _gui_trigger_full_reprocess(void *data)
{
  dt_develop_t *dev = (dt_develop_t *)data;
  if(dev) dt_dev_process_image(dev);
}
static void _gui_trigger_preview2_reprocess(void *data)
{
  dt_develop_t *dev = (dt_develop_t *)data;
  if(dev) dt_dev_process_preview2(dev);
}

/* Bounded Lua-thread sleep-poll for `pipe`'s DT_DEV_PIPE_ZOOMED bit to clear
   (see the long note above for why this bit, not preview_cb's timestamp
   check, is the correct freshness signal for a zoom/pan change). Re-issues
   the GUI-thread trigger once, halfway through the timeout, as a cheap
   defensive retry against the narrow accepted race documented above.
   Returns TRUE (fresh) or FALSE (timed out); always writes elapsed ms. */
static gboolean _wait_pipe_zoom_fresh(dt_dev_viewport_t *port, dt_dev_pixelpipe_t *pipe,
                                      int timeout_ms, int *waited_ms_out)
{
  const int step_ms = 15;
  int waited = 0;
  gboolean retried = FALSE;
  while(waited < timeout_ms)
  {
    const gboolean fresh = !(pipe->changed & DT_DEV_PIPE_ZOOMED) && pipe->status == DT_DEV_PIXELPIPE_VALID;
    if(fresh)
    {
      if(waited_ms_out) *waited_ms_out = waited;
      return TRUE;
    }
    if(!retried && waited >= timeout_ms / 2 && !pipe->processing)
    {
      retried = TRUE;
      dt_develop_t *dev = darktable.develop;
      if(dev)
      {
        if(port == &dev->full)
          _run_on_gui_sync(_gui_trigger_full_reprocess, dev);
        else
          _run_on_gui_sync(_gui_trigger_preview2_reprocess, dev);
      }
    }
    dt_lua_unlock();
    g_usleep((gulong)step_ms * 1000);
    dt_lua_lock();
    waited += step_ms;
  }
  if(waited_ms_out) *waited_ms_out = waited;
  return FALSE;
}

/* darktable.develop.set_viewport(viewport, zoom_x, zoom_y, scale
                                  [, wait_for_pipe][, timeout_ms])
   See the design note above. Registered WITHOUT dt_lua_gtk_wrap -- like
   preview_cb, the wait must run on the Lua thread, not the GUI thread.
   Returns {ok=true, viewport=, previous={zoom,zoom_label,closeup,zoom_x,
   zoom_y,scale}, applied={zoom_x,zoom_y,scale}, clamped=[{field="scale",
   requested=,clamped_to=,floor=,ceiling=}] (empty array if no clamp),
   pipe_ready=, waited_ms=} or {error=...} (includes "viewport_not_active:
   ..." when viewport="preview2" and the second window is not open). */
static int set_viewport_cb(lua_State *L)
{
  const char *viewport_name = luaL_checkstring(L, 1);
  const double zoom_x_arg = luaL_checknumber(L, 2);
  const double zoom_y_arg = luaL_checknumber(L, 3);
  const double scale_arg = luaL_checknumber(L, 4);
  const gboolean wait_for_pipe = lua_isnoneornil(L, 5) ? TRUE : lua_toboolean(L, 5);
  const int timeout_ms = luaL_optinteger(L, 6, 4000);

  if(g_strcmp0(viewport_name, "main") && g_strcmp0(viewport_name, "preview2"))
    return push_error(L, "viewport must be 'main' or 'preview2' (got '%s')", viewport_name);

  dt_lua_viewport_op_t s = { .viewport_name = viewport_name, .is_set = TRUE,
                            .mode = DT_ZOOM_FREE, .mode_closeup = 0,
                            .requested_scale = (float)scale_arg,
                            .target_zoom_x = (float)zoom_x_arg, .target_zoom_y = (float)zoom_y_arg };
  _run_on_gui_sync(_viewport_op_apply_gui, &s);

  if(s.viewport_not_active)
    return push_error(L, "viewport_not_active: preview2 window is not open");
  if(!s.ok)
    return push_error(L, "not in darkroom / no image loaded");

  int waited_ms = 0;
  gboolean pipe_ready = TRUE;
  if(wait_for_pipe)
    pipe_ready = _wait_pipe_zoom_fresh(s.port, s.pipe, timeout_ms, &waited_ms);

  lua_newtable(L);
  lua_pushboolean(L, TRUE); lua_setfield(L, -2, "ok");
  lua_pushstring(L, viewport_name); lua_setfield(L, -2, "viewport");

  lua_newtable(L);
  lua_pushinteger(L, (lua_Integer)s.prev_zoom); lua_setfield(L, -2, "zoom");
  lua_pushstring(L, _zoom_name(s.prev_zoom)); lua_setfield(L, -2, "zoom_label");
  lua_pushinteger(L, s.prev_closeup); lua_setfield(L, -2, "closeup");
  lua_pushnumber(L, s.prev_zoom_x); lua_setfield(L, -2, "zoom_x");
  lua_pushnumber(L, s.prev_zoom_y); lua_setfield(L, -2, "zoom_y");
  lua_pushnumber(L, s.prev_scale); lua_setfield(L, -2, "scale");
  lua_setfield(L, -2, "previous");

  lua_newtable(L);
  lua_pushnumber(L, s.target_zoom_x); lua_setfield(L, -2, "zoom_x");
  lua_pushnumber(L, s.target_zoom_y); lua_setfield(L, -2, "zoom_y");
  lua_pushnumber(L, s.applied_scale); lua_setfield(L, -2, "scale");
  lua_setfield(L, -2, "applied");

  lua_newtable(L); // clamped: array, empty when nothing was clamped
  if(s.scale_clamped)
  {
    lua_newtable(L);
    lua_pushstring(L, "scale"); lua_setfield(L, -2, "field");
    lua_pushnumber(L, scale_arg); lua_setfield(L, -2, "requested");
    lua_pushnumber(L, s.applied_scale); lua_setfield(L, -2, "clamped_to");
    lua_pushnumber(L, s.scale_floor); lua_setfield(L, -2, "floor");
    lua_pushnumber(L, s.scale_ceiling); lua_setfield(L, -2, "ceiling");
    lua_rawseti(L, -2, 1);
  }
  lua_setfield(L, -2, "clamped");

  lua_pushboolean(L, pipe_ready); lua_setfield(L, -2, "pipe_ready");
  lua_pushinteger(L, waited_ms); lua_setfield(L, -2, "waited_ms");

  return 1;
}

/* darktable.develop.restore_viewport(viewport, zoom, closeup, zoom_x,
                                      zoom_y, scale[, wait_for_pipe]
                                      [, timeout_ms])
   Reproduces an EXACT prior state -- the same {zoom,closeup,zoom_x,zoom_y,
   scale} tuple get_viewport()'s (and set_viewport()'s `previous`) fields
   already provide (requirement 7, "no new state needs inventing"). `zoom`
   is the raw integer enum (the `zoom` field, not `zoom_label`); it is
   ALWAYS one of DT_ZOOM_FIT/FILL/DT_ZOOM_1/FREE in practice (the only 4
   values ever actually stored into port->zoom -- see the design note
   above), so there is no sentinel-collision risk with DT_ZOOM_1's
   closeup==-1/-2 interactive-toggle branches (we always pass a concrete,
   non-negative closeup here). `scale` is used verbatim for FREE and is a
   harmless placeholder for FIT/FILL/DT_ZOOM_1 (those recompute their
   effective scale fresh on every read via dt_dev_get_zoom_scale's switch,
   ignoring the raw stored value). No clamping: a state that was once valid
   is restored as-is. Returns {ok=true, viewport=, pipe_ready=, waited_ms=}
   or {error=...}. */
static int restore_viewport_cb(lua_State *L)
{
  const char *viewport_name = luaL_checkstring(L, 1);
  const int zoom_arg = luaL_checkinteger(L, 2);
  const int closeup_arg = luaL_checkinteger(L, 3);
  const double zoom_x_arg = luaL_checknumber(L, 4);
  const double zoom_y_arg = luaL_checknumber(L, 5);
  const double scale_arg = luaL_checknumber(L, 6);
  const gboolean wait_for_pipe = lua_isnoneornil(L, 7) ? TRUE : lua_toboolean(L, 7);
  const int timeout_ms = luaL_optinteger(L, 8, 4000);

  if(g_strcmp0(viewport_name, "main") && g_strcmp0(viewport_name, "preview2"))
    return push_error(L, "viewport must be 'main' or 'preview2' (got '%s')", viewport_name);

  dt_lua_viewport_op_t s = { .viewport_name = viewport_name, .is_set = FALSE,
                            .mode = (dt_dev_zoom_t)zoom_arg, .mode_closeup = closeup_arg,
                            .requested_scale = (float)scale_arg,
                            .target_zoom_x = (float)zoom_x_arg, .target_zoom_y = (float)zoom_y_arg };
  _run_on_gui_sync(_viewport_op_apply_gui, &s);

  if(s.viewport_not_active)
    return push_error(L, "viewport_not_active: preview2 window is not open");
  if(!s.ok)
    return push_error(L, "not in darkroom / no image loaded");

  int waited_ms = 0;
  gboolean pipe_ready = TRUE;
  if(wait_for_pipe)
    pipe_ready = _wait_pipe_zoom_fresh(s.port, s.pipe, timeout_ms, &waited_ms);

  lua_newtable(L);
  lua_pushboolean(L, TRUE); lua_setfield(L, -2, "ok");
  lua_pushstring(L, viewport_name); lua_setfield(L, -2, "viewport");
  lua_pushboolean(L, pipe_ready); lua_setfield(L, -2, "pipe_ready");
  lua_pushinteger(L, waited_ms); lua_setfield(L, -2, "waited_ms");
  return 1;
}

/* GUI-thread snapshot + conditional (re)trigger of the preview worker.

   VIEWPORT SELECTOR (2026-07-31, extending set_viewport's own fix): `in`
   `viewport_name` picks WHICH pipe this grabs a backbuf from --
   NULL/""/"preview_pipe" (the original, default behavior, unchanged for
   every existing caller) reads dev->preview_pipe, a FIXED-native-resolution
   full-frame pipe entirely independent of any darkroom zoom (confirmed:
   dt_dev_process_preview_job_run processes it with port=NULL). "main"/
   "preview2" instead read dev->full.pipe / dev->preview2.pipe -- the SAME
   pipes set_viewport's dt_dev_zoom_move actually zooms, and the SAME ones
   the darkroom GUI itself paints the center view from (views/darkroom.c's
   expose path just cairo-paints this exact backbuf, scaled by zoom_scale --
   confirmed by reading dt_dev_pixelpipe_process's terminal backbuf write,
   pixelpipe_hb.c:3277-3309: it is UNCONDITIONAL for every pipe type except a
   distinct 4-float encoding for dt_pipe_is_screen, so dev->full.pipe gets a
   backbuf/backbuf_width/backbuf_height/backbuf_mutex populated the exact
   same way preview_pipe's already is). Because dt_dev_process_image_job
   computes x,y,wd,ht FROM THE CURRENT zoom_x/zoom_y/scale (develop.c:
   769-784) and wd/ht are capped by the WINDOW size (not by any fixed
   preview budget), a tight zoom devotes the window's ENTIRE fixed pixel
   budget to a small region -- this is the actual mechanism that makes
   "zoom in for more detail" work, and it is what was MISSING before: this
   binding used to always read preview_pipe regardless of viewport, so
   set_viewport's zoom changed WHAT get_viewport().region reported but
   never what capture_viewport's pixels actually were.

   FRESHNESS is likewise pipe-appropriate: content edits (dt_dev_add_
   history_item -> dt_dev_invalidate_all, develop.c:314) mark ALL THREE
   pipes DIRTY and bump dev->timestamp UNIFORMLY, so the timestamp check
   below is already correct for main/preview2 exactly as it always was for
   preview_pipe. But a PURE ZOOM/PAN change (dt_dev_zoom_move) does NOT
   bump dev->timestamp at all (only content edits do) -- it only sets
   port->pipe->changed |= DT_DEV_PIPE_ZOOMED (develop.c:3388), cleared once
   dt_dev_process_image_job actually re-runs at the new zoom (develop.c:
   788,837-858). So for main/preview2 this ALSO requires that bit clear --
   exactly the same freshness signal set_viewport_cb's own wait_for_pipe
   already uses, and for the same reason (see the long note above
   _viewport_op_apply_gui). preview_pipe never has this bit touched (zoom
   never applies to it), so the extra check is a harmless no-op there --
   gated on check_zoomed anyway to keep the two cases visibly distinct. */
typedef struct
{
  const char *viewport_name; // in: NULL/""/"preview_pipe" (default), "main", "preview2"
  gboolean in_darkroom;       // out: dev in darkroom with an image loaded
  gboolean viewport_not_active; // out: preview2 requested but its window is not open
  gboolean check_zoomed;      // out: TRUE for main/preview2 (needs the extra freshness bit)
  int want_ts;                // out: dev->timestamp we require the frame to match
  dt_dev_pixelpipe_t *pp;     // out: the resolved pipe pointer (stable while in darkroom)
} preview_snapshot_t;

static void _preview_snapshot_gui(void *data)
{
  preview_snapshot_t *s = (preview_snapshot_t *)data;
  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->gui_attached || !dt_is_valid_imgid(dev->image_storage.id))
  {
    s->in_darkroom = FALSE;
    return;
  }

  dt_dev_pixelpipe_t *pp = NULL;
  void (*trigger)(dt_develop_t *) = NULL;
  gboolean check_zoomed = FALSE;

  const gboolean is_default = !s->viewport_name || !s->viewport_name[0]
                              || !g_strcmp0(s->viewport_name, "preview_pipe");
  if(is_default)
  {
    if(!dev->preview_pipe) { s->in_darkroom = FALSE; return; }
    pp = dev->preview_pipe;
    trigger = dt_dev_process_preview;
  }
  else if(!g_strcmp0(s->viewport_name, "main"))
  {
    pp = dev->full.pipe;
    trigger = dt_dev_process_image;
    check_zoomed = TRUE;
  }
  else if(!g_strcmp0(s->viewport_name, "preview2"))
  {
    const gboolean p2_active = (dev->second_wnd != NULL) && (dev->preview2.widget != NULL);
    if(!p2_active)
    {
      s->in_darkroom = FALSE;
      s->viewport_not_active = TRUE;
      return;
    }
    pp = dev->preview2.pipe;
    trigger = dt_dev_process_preview2;
    check_zoomed = TRUE;
  }
  else
  {
    s->in_darkroom = FALSE; // unknown name -- defensive only, preview_cb already validates
    return;
  }

  if(!pp)
  {
    s->in_darkroom = FALSE;
    return;
  }

  s->in_darkroom = TRUE;
  s->pp = pp;
  s->check_zoomed = check_zoomed;
  s->want_ts = (int)dev->timestamp;
  // Only kick a fresh worker if the current backbuf is not already fresh
  // for BOTH signals that matter for this pipe; avoids a redundant re-render
  // on a plain (no preceding set_params/set_viewport) preview() call.
  const gboolean already_fresh =
    pp->status == DT_DEV_PIXELPIPE_VALID && pp->input_timestamp >= s->want_ts
    && (!check_zoomed || !(pp->changed & DT_DEV_PIPE_ZOOMED));
  if(!already_fresh)
    trigger(dev);
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

  // VIEWPORT (2026-07-31, arg 7, optional): "main" | "preview2" read from
  // dev->full.pipe / dev->preview2.pipe instead of the default
  // dev->preview_pipe -- see the long note on preview_snapshot_t/
  // _preview_snapshot_gui above for why this is the fix that makes
  // set_viewport's zoom actually show up here. Omitted (or nil): ZERO
  // change from the original preview_pipe behavior, for every existing
  // caller (get_preview's grain/sharpen/noise-inspection use, etc). When
  // given, `region` above (if also given) is relative to what THIS pipe's
  // backbuf currently shows -- for "main"/"preview2" while zoomed in, that
  // is the CURRENT on-screen crop, not the full image (exactly the same
  // "relative to the frame the pipe produced" contract this parameter
  // already had for preview_pipe -- no new semantics, just a different pipe).
  const char *viewport_name = lua_isnoneornil(L, 7) ? NULL : luaL_checkstring(L, 7);
  if(viewport_name && g_strcmp0(viewport_name, "main") && g_strcmp0(viewport_name, "preview2"))
    return push_error(L, "viewport must be 'main' or 'preview2' (or omitted for preview_pipe)");

  // (a) GUI-thread snapshot + conditional (re)trigger of the preview worker.
  preview_snapshot_t snap = { .viewport_name = viewport_name, .in_darkroom = FALSE,
                              .viewport_not_active = FALSE, .check_zoomed = FALSE,
                              .want_ts = 0, .pp = NULL };
  _run_on_gui_sync(_preview_snapshot_gui, &snap);

  if(snap.viewport_not_active)
    return push_error(L, "viewport_not_active: preview2 window is not open");
  if(!snap.in_darkroom || !snap.pp)
    return push_error(L, "not in darkroom / no image loaded");

  dt_dev_pixelpipe_t *pp = snap.pp;
  const int want_ts = snap.want_ts;
  const gboolean check_zoomed = snap.check_zoomed;

  // (b) bounded sleep-poll on the LUA thread for a FRESH VALID frame (§3.4
  // timeout 2000 ms). Sleeping here releases the Lua lock and never touches the
  // GUI main loop, so the GUI thread + worker run freely to completion.
  const int timeout_ms = 2000;
  const int step_ms = 15;
  int waited = 0;
  gboolean fresh = FALSE;
  while(waited < timeout_ms)
  {
    if(pp->status == DT_DEV_PIXELPIPE_VALID && pp->input_timestamp >= want_ts
       && (!check_zoomed || !(pp->changed & DT_DEV_PIPE_ZOOMED)))
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
  // Which pipe this actually came from -- never ambiguous which source a
  // given render's detail level should be attributed to.
  lua_pushstring(L, viewport_name ? viewport_name : "preview_pipe");
  lua_setfield(L, -2, "viewport_source");
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
  // feather is now a FRACTION of the mask bbox (see API 7 note), default 2%.
  const double feather_arg = luaL_optnumber(L, 5, 0.02);
  // smooth boundary via Catmull-Rom bezier handles; default on. Pass false for
  // the legacy faceted (straight-segment) polygon.
  const int smooth = lua_isnoneornil(L, 6) ? 1 : lua_toboolean(L, 6);

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

  // First pass: read + clamp every node into a plain C array, and track the
  // polygon's bounding box. We need all corners up front to (a) derive smooth
  // Catmull-Rom bezier handles from each node's neighbors and (b) scale the
  // feather to the mask's OWN size instead of a frame constant. On ANY
  // malformed node, free and report a graceful error (never crash, #4).
  float *cx = g_malloc_n(npoints, sizeof(float));
  float *cy = g_malloc_n(npoints, sizeof(float));
  float minx = 1.0f, miny = 1.0f, maxx = 0.0f, maxy = 0.0f;
  for(int i = 0; i < npoints; i++)
  {
    lua_rawgeti(L, 3, i + 1); // point table (1-based)
    if(lua_type(L, -1) != LUA_TTABLE)
    {
      lua_pop(L, 1);
      g_free(cx); g_free(cy);
      return push_error(L, "point %d is not a table {x=..,y=..}", i + 1);
    }
    lua_getfield(L, -1, "x");
    lua_getfield(L, -2, "y");
    if(!lua_isnumber(L, -2) || !lua_isnumber(L, -1))
    {
      lua_pop(L, 3);
      g_free(cx); g_free(cy);
      return push_error(L, "point %d has non-numeric x/y", i + 1);
    }
    double x = lua_tonumber(L, -2);
    double y = lua_tonumber(L, -1);
    lua_pop(L, 3); // y, x, point table

    if(x < 0.0) x = 0.0;
    if(x > 1.0) x = 1.0;
    if(y < 0.0) y = 0.0;
    if(y > 1.0) y = 1.0;
    cx[i] = (float)x;
    cy[i] = (float)y;
    if(cx[i] < minx) minx = cx[i];
    if(cx[i] > maxx) maxx = cx[i];
    if(cy[i] < miny) miny = cy[i];
    if(cy[i] > maxy) maxy = cy[i];
  }

  // feather (soft edge) as a fraction of the mask's bbox max dimension, so a
  // small subject gets a proportionally small edge instead of a frame-constant
  // halo. Floored so a degenerate/near-zero bbox still yields a valid border.
  const float bbox_dim = MAX(maxx - minx, maxy - miny);
  const float border = MAX(0.0005f, (float)feather * MAX(bbox_dim, 1e-4f));

  // Build the polygon form.
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_PATH);
  if(!form)
  {
    g_free(cx); g_free(cy);
    return push_error(L, "dt_masks_create failed");
  }

  for(int i = 0; i < npoints; i++)
  {
    dt_masks_point_path_t *bzpt = malloc(sizeof(dt_masks_point_path_t));
    if(!bzpt)
    {
      dt_masks_free_form(form);
      g_free(cx); g_free(cy);
      return push_error(L, "out of memory building path node");
    }
    bzpt->corner[0] = cx[i];
    bzpt->corner[1] = cy[i];
    if(smooth && npoints >= 3)
    {
      // Catmull-Rom -> cubic bezier control points, closed loop (indices wrap).
      // Matches darktable's own _path_catmull_to_bezier (masks/path.c): incoming
      // handle ctrl1 = (C[i-1] + 6*C[i] - C[i+1])/6, outgoing handle
      // ctrl2 = (-C[i-1] + 6*C[i] + C[i+1])/6. Explicit handles + USER state keep
      // the smooth shape stable through the blend/eval path (which does NOT
      // resolve the -1 auto-sentinel), so no more faceted outline on curves.
      const int pv = (i - 1 + npoints) % npoints;
      const int nx = (i + 1) % npoints;
      bzpt->ctrl1[0] = (cx[pv] + 6.0f * cx[i] - cx[nx]) / 6.0f;
      bzpt->ctrl1[1] = (cy[pv] + 6.0f * cy[i] - cy[nx]) / 6.0f;
      bzpt->ctrl2[0] = (-cx[pv] + 6.0f * cx[i] + cx[nx]) / 6.0f;
      bzpt->ctrl2[1] = (-cy[pv] + 6.0f * cy[i] + cy[nx]) / 6.0f;
    }
    else
    {
      // sharp corner: ctrl == corner -> straight edges (legacy faceted shape).
      bzpt->ctrl1[0] = bzpt->ctrl2[0] = cx[i];
      bzpt->ctrl1[1] = bzpt->ctrl2[1] = cy[i];
    }
    bzpt->border[0] = bzpt->border[1] = border;
    bzpt->state = DT_MASKS_POINT_STATE_USER;
    form->points = g_list_append(form->points, bzpt);
  }
  g_free(cx);
  g_free(cy);

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
  lua_pushboolean(L, smooth);                  lua_setfield(L, -2, "smooth");
  lua_pushinteger(L, (lua_Integer)module->blend_params->mask_mode);
  lua_setfield(L, -2, "mask_mode");
  return 1;
}

/* ---- Retouch module: local heal/clone/blur/fill shapes ------------------
   Distinct from add_path_mask's generic "restrict this module's blended
   output to a region": each retouch shape ties a mask FORM to a PER-SHAPE
   entry in the retouch module's OWN dt_iop_retouch_params_t.rt_forms[]
   array (iop/retouch.h, extracted from iop/retouch.c so this file can see
   the layout) carrying that shape's algorithm + wavelet scale; heal/clone
   additionally store an absolute normalized source point directly on the
   form (dt_masks_form_t.source[0,1], develop/masks.h) -- confirmed NOT a
   delta by develop/masks/circle.c's own source-point code.

   retouch.c's rt_resynch_params (which normally keeps rt_forms[] in sync
   with the mask group) is `static`, unreachable from here, and only ever
   runs from gui_update() -- itself only called if module->expander is set.
   Pixel processing (rt_compute_roi_in, iop/retouch.c) reads rt_forms[]
   directly by formid, not through rt_resynch_params, so every binding below
   writes/removes its own rt_forms[] entry directly instead of depending on
   a GUI refresh -- correct whether or not the retouch panel happens to be
   expanded when MCP acts. Also: unlike add_path_mask, these do NOT touch
   blend_params->mask_mode -- retouch's own processing keys off
   blend_params->mask_id directly; the mask_mode restrict-blend bits are an
   unrelated generic-blend feature and setting them here would be wrong. */

static gboolean rt_algo_from_string(const char *s, dt_iop_retouch_algo_type_t *out)
{
  if(!s) return FALSE;
  if(!g_strcmp0(s, "heal")) { *out = DT_IOP_RETOUCH_HEAL; return TRUE; }
  if(!g_strcmp0(s, "clone")) { *out = DT_IOP_RETOUCH_CLONE; return TRUE; }
  return FALSE;
}

static const char *rt_algo_to_string(dt_iop_retouch_algo_type_t a)
{
  switch(a)
  {
    case DT_IOP_RETOUCH_CLONE: return "clone";
    case DT_IOP_RETOUCH_HEAL:  return "heal";
    case DT_IOP_RETOUCH_BLUR:  return "blur";
    case DT_IOP_RETOUCH_FILL:  return "fill";
    default: return "none";
  }
}

/* darktable.develop.retouch_add_shape(op, instance, algorithm, target_x,
 * target_y, radius, feather, source_x, source_y[, scale][, opacity])
 * ===========================================================================
 * Create a CIRCLE retouch shape (heal or clone; only shape/algorithms
 * supported in this build -- see the module doc comment above) and bind it
 * to a wavelet scale. Runs on the GUI thread (dt_lua_gtk_wrap).
 *
 * ARGS
 *   op, instance   must resolve to the "retouch" module -- this binding
 *                  assumes the retouch params layout, unlike add_path_mask.
 *   algorithm      "heal" or "clone".
 *   target_x/y     shape center, normalized [0,1] image coords, clamped.
 *   radius/feather normalized, clamped to [0.0005, 0.5] / [0, 0.5].
 *   source_x/y     required (heal/clone always need a source point).
 *   scale          optional wavelet scale, 0..RETOUCH_MAX_SCALES+1 (residual
 *                  slot); default = the module's current curr_scale.
 *   opacity        optional 0..1, default 1.0.
 *
 * Returns {ok=true, op, instance, formid, mask_id, algorithm, shape_type,
 * target={x,y}, source={x,y}, radius, feather, opacity, wavelet_scale} or
 * {error=...}. */
static int retouch_add_shape_cb(lua_State *L)
{
  const char *op = luaL_checkstring(L, 1);
  const int instance = luaL_optinteger(L, 2, 0);
  const char *algo_str = luaL_checkstring(L, 3);
  const double target_x_arg = luaL_checknumber(L, 4);
  const double target_y_arg = luaL_checknumber(L, 5);
  const double radius_arg = luaL_checknumber(L, 6);
  const double feather_arg = luaL_checknumber(L, 7);
  if(lua_isnoneornil(L, 8) || lua_isnoneornil(L, 9))
    return push_error(L, "retouch_add_shape: source_x/source_y required for heal/clone");
  const double source_x_arg = luaL_checknumber(L, 8);
  const double source_y_arg = luaL_checknumber(L, 9);
  const int scale_arg = luaL_optinteger(L, 10, -1); // -1 => default to curr_scale
  const double opacity_arg = luaL_optnumber(L, 11, 1.0);

  dt_iop_retouch_algo_type_t algorithm;
  if(!rt_algo_from_string(algo_str, &algorithm))
    return push_error(L, "retouch_add_shape: unsupported algorithm '%s' "
                          "(only 'heal'/'clone' in this build)", algo_str);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  dt_iop_module_t *module = _find_module(dev, op, instance);
  if(!module)
    return push_error(L, "no module '%s' instance %d in current darkroom", op, instance);
  if(g_strcmp0(module->op, "retouch"))
    return push_error(L, "retouch_add_shape: module '%s' is not the retouch module", op);

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)module->params;

  // find a free rt_forms[] slot (mirrors retouch.c's static rt_allow_create_form cap check).
  int slot = -1;
  for(int i = 0; i < RETOUCH_NO_FORMS; i++)
  {
    if(!dt_is_valid_maskid(p->rt_forms[i].formid)) { slot = i; break; }
  }
  if(slot < 0)
    return push_error(L, "retouch_add_shape: %d shape limit reached for this instance", RETOUCH_NO_FORMS);

  const double target_x = CLAMPS(target_x_arg, 0.0, 1.0);
  const double target_y = CLAMPS(target_y_arg, 0.0, 1.0);
  const double source_x = CLAMPS(source_x_arg, 0.0, 1.0);
  const double source_y = CLAMPS(source_y_arg, 0.0, 1.0);
  const double radius = CLAMPS(radius_arg, 0.0005, 0.5);
  const double feather = CLAMPS(feather_arg, 0.0, 0.5);
  const double opacity = CLAMPS(opacity_arg, 0.0, 1.0);
  const int scale = (scale_arg < 0) ? p->curr_scale : (int)CLAMPS(scale_arg, 0, RETOUCH_MAX_SCALES + 1);

  dt_masks_form_t *form = dt_masks_create(DT_MASKS_CIRCLE | DT_MASKS_CLONE);
  if(!form)
    return push_error(L, "dt_masks_create failed");

  dt_masks_point_circle_t *circle = malloc(sizeof(dt_masks_point_circle_t));
  if(!circle)
  {
    dt_masks_free_form(form);
    return push_error(L, "out of memory building circle shape");
  }
  circle->center[0] = (float)target_x;
  circle->center[1] = (float)target_y;
  circle->radius = (float)radius;
  circle->border = (float)feather;
  form->points = g_list_append(form->points, circle);

  form->source[0] = (float)source_x;
  form->source[1] = (float)source_y;
  form->source[2] = 0.0f;

  // WIRE: name form, append to dev->forms, create/extend the module's mask
  // group (sets blend_params->mask_id), add group point, masks-history item.
  dt_masks_gui_form_save_creation(dev, module, form, NULL);

  const dt_mask_id_t formid = form->formid;
  const dt_mask_id_t mask_id = module->blend_params->mask_id;

  // override the group point's opacity to the requested value (save_creation
  // seeds it from a conf default) -- same pattern as add_path_mask_cb.
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
    dt_dev_add_masks_history_item(dev, module, TRUE);
  }

  // write the rt_forms[] entry DIRECTLY -- see the module doc comment above
  // for why this must not depend on rt_resynch_params.
  memset(&p->rt_forms[slot], 0, sizeof(dt_iop_retouch_form_data_t));
  p->rt_forms[slot].formid = formid;
  p->rt_forms[slot].scale = scale;
  p->rt_forms[slot].algorithm = algorithm;
  p->rt_forms[slot].distort_mode = 2;

  // commit params + live reprocess (invalidates the pipe so the next
  // preview() reflects the new shape).
  dt_dev_add_history_item(dev, module, module->enabled);

  if(module->expander) dt_iop_gui_update(module);

  lua_newtable(L);
  lua_pushboolean(L, TRUE);                        lua_setfield(L, -2, "ok");
  lua_pushstring(L, module->op);                   lua_setfield(L, -2, "op");
  lua_pushinteger(L, module->multi_priority);      lua_setfield(L, -2, "instance");
  lua_pushinteger(L, (lua_Integer)formid);         lua_setfield(L, -2, "formid");
  lua_pushinteger(L, (lua_Integer)mask_id);        lua_setfield(L, -2, "mask_id");
  lua_pushstring(L, rt_algo_to_string(algorithm)); lua_setfield(L, -2, "algorithm");
  lua_pushstring(L, "circle");                     lua_setfield(L, -2, "shape_type");
  lua_newtable(L);
  lua_pushnumber(L, target_x); lua_setfield(L, -2, "x");
  lua_pushnumber(L, target_y); lua_setfield(L, -2, "y");
  lua_setfield(L, -2, "target");
  lua_newtable(L);
  lua_pushnumber(L, source_x); lua_setfield(L, -2, "x");
  lua_pushnumber(L, source_y); lua_setfield(L, -2, "y");
  lua_setfield(L, -2, "source");
  lua_pushnumber(L, radius);   lua_setfield(L, -2, "radius");
  lua_pushnumber(L, feather);  lua_setfield(L, -2, "feather");
  lua_pushnumber(L, opacity);  lua_setfield(L, -2, "opacity");
  lua_pushinteger(L, scale);   lua_setfield(L, -2, "wavelet_scale");
  return 1;
}

/* darktable.develop.retouch_update_shape(op, instance, formid, target_x,
 * target_y, radius, feather, source_x, source_y[, algorithm][, scale][, opacity])
 * ===========================================================================
 * Move/resize an EXISTING retouch circle shape IN PLACE (same formid) --
 * unlike delete+retouch_add_shape, this does not reallocate a new form or
 * rt_forms[] slot, so a scripted "tune this point" edit doesn't churn
 * formids on every adjustment. Mirrors circle.c's own drag-commit path
 * (_circle_events_button_released): geometry commits via
 * dt_dev_add_masks_history_item (masks-history), NOT dt_dev_add_history_item
 * (iop-params-history) -- the latter is only needed for rt_forms[]'s
 * algorithm/scale, which live in module->params, not in the mask geometry.
 * No dt_dev_distort_backtransform is needed (unlike the GUI drag path) since
 * we already write normalized image coords directly, not on-screen pixels.
 * Runs on the GUI thread (dt_lua_gtk_wrap).
 *
 * ARGS
 *   formid                     existing shape id (from retouch_add_shape or
 *                               retouch_list_shapes); must already be a
 *                               member of this module instance's mask group.
 *   target_x/y, radius, feather,
 *   source_x/y                 same semantics/clamping as retouch_add_shape.
 *                               Always resent in full -- this is a "move",
 *                               not a partial field patch.
 *   algorithm                   optional "heal"/"clone"; omit/nil to keep
 *                               the shape's current algorithm.
 *   scale                       optional wavelet scale; omit or negative to
 *                               keep the shape's current scale.
 *   opacity                     optional 0..1; omit/nil to leave the mask
 *                               group's opacity untouched.
 *
 * Returns {ok=true, op, instance, formid, mask_id, algorithm, shape_type,
 * target={x,y}, source={x,y}, radius, feather, opacity, wavelet_scale} or
 * {error=...}. */
static int retouch_update_shape_cb(lua_State *L)
{
  const char *op = luaL_checkstring(L, 1);
  const int instance = luaL_optinteger(L, 2, 0);
  const dt_mask_id_t formid = (dt_mask_id_t)luaL_checkinteger(L, 3);
  const double target_x_arg = luaL_checknumber(L, 4);
  const double target_y_arg = luaL_checknumber(L, 5);
  const double radius_arg = luaL_checknumber(L, 6);
  const double feather_arg = luaL_checknumber(L, 7);
  if(lua_isnoneornil(L, 8) || lua_isnoneornil(L, 9))
    return push_error(L, "retouch_update_shape: source_x/source_y required");
  const double source_x_arg = luaL_checknumber(L, 8);
  const double source_y_arg = luaL_checknumber(L, 9);
  const char *algo_str = lua_isnoneornil(L, 10) ? NULL : luaL_checkstring(L, 10);
  const int scale_arg = luaL_optinteger(L, 11, -1);
  const gboolean has_opacity = !lua_isnoneornil(L, 12);
  const double opacity_arg = luaL_optnumber(L, 12, 1.0);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  dt_iop_module_t *module = _find_module(dev, op, instance);
  if(!module)
    return push_error(L, "no module '%s' instance %d in current darkroom", op, instance);
  if(g_strcmp0(module->op, "retouch"))
    return push_error(L, "retouch_update_shape: module '%s' is not the retouch module", op);

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)module->params;

  dt_masks_form_t *grp = dt_masks_get_from_id(dev, module->blend_params->mask_id);
  if(!grp)
    return push_error(L, "retouch_update_shape: module has no shapes group");

  dt_masks_point_group_t *member = NULL;
  for(GList *l = grp->points; l; l = g_list_next(l))
  {
    dt_masks_point_group_t *gp = (dt_masks_point_group_t *)l->data;
    if(gp && gp->formid == formid) { member = gp; break; }
  }
  if(!member)
    return push_error(L, "retouch_update_shape: formid %d is not a shape on module '%s'",
                       (int)formid, op);

  dt_masks_form_t *form = dt_masks_get_from_id(dev, formid);
  if(!form)
    return push_error(L, "retouch_update_shape: formid %d not found", (int)formid);
  if(!(form->type & DT_MASKS_CIRCLE) || !form->points)
    return push_error(L, "retouch_update_shape: formid %d is not a circle shape", (int)formid);

  int slot = -1;
  for(int i = 0; i < RETOUCH_NO_FORMS; i++)
  {
    if(p->rt_forms[i].formid == formid) { slot = i; break; }
  }
  if(slot < 0)
    return push_error(L, "retouch_update_shape: formid %d has no rt_forms[] entry", (int)formid);

  dt_iop_retouch_algo_type_t algorithm = p->rt_forms[slot].algorithm;
  if(algo_str && !rt_algo_from_string(algo_str, &algorithm))
    return push_error(L, "retouch_update_shape: unsupported algorithm '%s' "
                          "(only 'heal'/'clone' in this build)", algo_str);

  const double target_x = CLAMPS(target_x_arg, 0.0, 1.0);
  const double target_y = CLAMPS(target_y_arg, 0.0, 1.0);
  const double source_x = CLAMPS(source_x_arg, 0.0, 1.0);
  const double source_y = CLAMPS(source_y_arg, 0.0, 1.0);
  const double radius = CLAMPS(radius_arg, 0.0005, 0.5);
  const double feather = CLAMPS(feather_arg, 0.0, 0.5);
  const int scale = (scale_arg < 0)
      ? p->rt_forms[slot].scale
      : (int)CLAMPS(scale_arg, 0, RETOUCH_MAX_SCALES + 1);

  // geometry: direct mutation of the live form, same fields circle.c's
  // button-released drag-commit writes.
  dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)form->points->data;
  circle->center[0] = (float)target_x;
  circle->center[1] = (float)target_y;
  circle->radius = (float)radius;
  circle->border = (float)feather;
  form->source[0] = (float)source_x;
  form->source[1] = (float)source_y;

  double opacity = member->opacity;
  if(has_opacity)
  {
    opacity = CLAMPS(opacity_arg, 0.0, 1.0);
    member->opacity = (float)opacity;
  }
  dt_dev_add_masks_history_item(dev, module, TRUE);

  // rt_forms[] direct write -- see the module doc comment near
  // retouch_add_shape_cb for why this must not depend on rt_resynch_params.
  p->rt_forms[slot].algorithm = algorithm;
  p->rt_forms[slot].scale = scale;
  dt_dev_add_history_item(dev, module, module->enabled);

  if(module->expander) dt_iop_gui_update(module);

  lua_newtable(L);
  lua_pushboolean(L, TRUE);                        lua_setfield(L, -2, "ok");
  lua_pushstring(L, module->op);                   lua_setfield(L, -2, "op");
  lua_pushinteger(L, module->multi_priority);      lua_setfield(L, -2, "instance");
  lua_pushinteger(L, (lua_Integer)formid);         lua_setfield(L, -2, "formid");
  lua_pushinteger(L, (lua_Integer)module->blend_params->mask_id);
  lua_setfield(L, -2, "mask_id");
  lua_pushstring(L, rt_algo_to_string(algorithm)); lua_setfield(L, -2, "algorithm");
  lua_pushstring(L, "circle");                     lua_setfield(L, -2, "shape_type");
  lua_newtable(L);
  lua_pushnumber(L, target_x); lua_setfield(L, -2, "x");
  lua_pushnumber(L, target_y); lua_setfield(L, -2, "y");
  lua_setfield(L, -2, "target");
  lua_newtable(L);
  lua_pushnumber(L, source_x); lua_setfield(L, -2, "x");
  lua_pushnumber(L, source_y); lua_setfield(L, -2, "y");
  lua_setfield(L, -2, "source");
  lua_pushnumber(L, radius);   lua_setfield(L, -2, "radius");
  lua_pushnumber(L, feather);  lua_setfield(L, -2, "feather");
  lua_pushnumber(L, opacity);  lua_setfield(L, -2, "opacity");
  lua_pushinteger(L, scale);   lua_setfield(L, -2, "wavelet_scale");
  return 1;
}

/* darktable.develop.retouch_delete_shape(op, instance, formid)
 * ===========================================================================
 * Remove a retouch shape (mask form + its rt_forms[] entry). Runs on the GUI
 * thread. Manually compacts rt_forms[] instead of relying on
 * rt_resynch_params (see the module doc comment above), so deletion is
 * correct headless.
 *
 * Returns {ok=true} or {error=...}. */
static int retouch_delete_shape_cb(lua_State *L)
{
  const char *op = luaL_checkstring(L, 1);
  const int instance = luaL_optinteger(L, 2, 0);
  const dt_mask_id_t formid = (dt_mask_id_t)luaL_checkinteger(L, 3);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  dt_iop_module_t *module = _find_module(dev, op, instance);
  if(!module)
    return push_error(L, "no module '%s' instance %d in current darkroom", op, instance);
  if(g_strcmp0(module->op, "retouch"))
    return push_error(L, "retouch_delete_shape: module '%s' is not the retouch module", op);

  dt_iop_retouch_params_t *p = (dt_iop_retouch_params_t *)module->params;

  dt_masks_form_t *grp = dt_masks_get_from_id(dev, module->blend_params->mask_id);
  if(!grp)
    return push_error(L, "retouch_delete_shape: module has no shapes group");

  gboolean is_member = FALSE;
  for(GList *l = grp->points; l; l = g_list_next(l))
  {
    dt_masks_point_group_t *gp = (dt_masks_point_group_t *)l->data;
    if(gp && gp->formid == formid) { is_member = TRUE; break; }
  }
  if(!is_member)
    return push_error(L, "retouch_delete_shape: formid %d is not a shape on module '%s'",
                       (int)formid, op);

  dt_masks_form_t *form = dt_masks_get_from_id(dev, formid);
  if(!form)
    return push_error(L, "retouch_delete_shape: formid %d not found", (int)formid);

  dt_masks_form_remove(module, grp, form);

  // manually compact rt_forms[] -- see the module doc comment above.
  int found = -1;
  for(int i = 0; i < RETOUCH_NO_FORMS; i++)
  {
    if(p->rt_forms[i].formid == formid) { found = i; break; }
  }
  if(found >= 0)
  {
    for(int i = found; i < RETOUCH_NO_FORMS - 1; i++)
      p->rt_forms[i] = p->rt_forms[i + 1];
    memset(&p->rt_forms[RETOUCH_NO_FORMS - 1], 0, sizeof(dt_iop_retouch_form_data_t));
  }

  dt_dev_add_history_item(dev, module, module->enabled);
  if(module->expander) dt_iop_gui_update(module);

  lua_newtable(L);
  lua_pushboolean(L, TRUE); lua_setfield(L, -2, "ok");
  return 1;
}

/* darktable.develop.retouch_list_shapes(op, instance)
 * ===========================================================================
 * Read-only: the real, currently-attached retouch shapes for a module
 * instance (cross-referencing the mask group's points with rt_forms[] by
 * formid) -- NOT the raw 300-slot rt_forms array the generic get_params
 * introspection would otherwise expose full of mostly-NO_MASKID padding.
 * Also returns the module's wavelet-scale state.
 *
 * COORDINATE FRAMES: target/source/radius/feather are dt_masks' OWN storage
 * values, i.e. normalized against the PIPE-INPUT/mask frame (radius/feather
 * against mindim(iwidth,iheight)). Those are the values retouch_add_shape/
 * retouch_update_shape take back, so a read-modify-write round trip is exact.
 * They are NOT the frame get_viewport()/get_preview()/capture_viewport render
 * in -- so each shape ALSO carries target_display/source_display/
 * radius_display/feather_display (see _mask_point_to_display /
 * _mask_len_to_display), which are directly plottable on top of a captured
 * render. Reporting only the mask frame made "is my shape where I aimed it?"
 * unanswerable for any portrait/cropped image; reporting both removes the
 * guess. The *_display fields are omitted (not zeroed) when no processed pipe
 * exists yet or distort_transform fails.
 *
 * opacity is the mask GROUP MEMBER's opacity (dt_masks_point_group_t.opacity,
 * what retouch_add_shape/update_shape's `opacity` arg writes), not the
 * module's blend opacity.
 *
 * Returns {module="retouch", instance, num_scales, curr_scale,
 * merge_from_scale, shapes={{formid, algorithm, shape_type, target={x,y},
 * source={x,y} (heal/clone only), radius, feather, opacity, wavelet_scale,
 * target_display={x,y}, source_display={x,y}, radius_display,
 * feather_display}, ...}} or {error=...}. */
static int retouch_list_shapes_cb(lua_State *L)
{
  const char *op = luaL_checkstring(L, 1);
  const int instance = luaL_optinteger(L, 2, 0);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  dt_iop_module_t *module = _find_module(dev, op, instance);
  if(!module)
    return push_error(L, "no module '%s' instance %d in current darkroom", op, instance);
  if(g_strcmp0(module->op, "retouch"))
    return push_error(L, "retouch_list_shapes: module '%s' is not the retouch module", op);

  const dt_iop_retouch_params_t *p = (const dt_iop_retouch_params_t *)module->params;

  lua_newtable(L);
  lua_pushstring(L, "retouch");                lua_setfield(L, -2, "module");
  lua_pushinteger(L, module->multi_priority);  lua_setfield(L, -2, "instance");
  lua_pushinteger(L, p->num_scales);           lua_setfield(L, -2, "num_scales");
  lua_pushinteger(L, p->curr_scale);           lua_setfield(L, -2, "curr_scale");
  lua_pushinteger(L, p->merge_from_scale);     lua_setfield(L, -2, "merge_from_scale");

  // dims for the mask -> display frame conversion (COORDINATE FRAMES note
  // above). All zero while no processed pipe exists yet, in which case every
  // *_display field is simply omitted rather than reported as a bogus 0.
  float wd = 0.0f, ht = 0.0f, iwidth = 0.0f, iheight = 0.0f;
  dt_masks_get_image_size(&wd, &ht, &iwidth, &iheight);
  const gboolean have_display =
    (wd > 0.0f && ht > 0.0f && iwidth > 0.0f && iheight > 0.0f);
  lua_pushboolean(L, have_display); lua_setfield(L, -2, "has_display_frame");

  lua_newtable(L);
  int out_index = 1;

  dt_masks_form_t *grp = dt_masks_get_from_id(dev, module->blend_params->mask_id);
  if(grp && (grp->type & DT_MASKS_GROUP))
  {
    for(GList *l = grp->points; l; l = g_list_next(l))
    {
      dt_masks_point_group_t *gp = (dt_masks_point_group_t *)l->data;
      if(!gp) continue;
      const dt_mask_id_t formid = gp->formid;

      int rt_index = -1;
      for(int i = 0; i < RETOUCH_NO_FORMS; i++)
      {
        if(p->rt_forms[i].formid == formid) { rt_index = i; break; }
      }
      if(rt_index < 0) continue; // not a retouch shape entry; skip defensively

      dt_masks_form_t *form = dt_masks_get_from_id(dev, formid);
      if(!form) continue;

      lua_newtable(L);
      lua_pushinteger(L, (lua_Integer)formid); lua_setfield(L, -2, "formid");
      lua_pushstring(L, rt_algo_to_string(p->rt_forms[rt_index].algorithm));
      lua_setfield(L, -2, "algorithm");
      lua_pushinteger(L, p->rt_forms[rt_index].scale); lua_setfield(L, -2, "wavelet_scale");
      lua_pushnumber(L, gp->opacity); lua_setfield(L, -2, "opacity");

      if(form->type & DT_MASKS_CIRCLE)
      {
        lua_pushstring(L, "circle"); lua_setfield(L, -2, "shape_type");
        dt_masks_point_circle_t *circle =
          form->points ? (dt_masks_point_circle_t *)form->points->data : NULL;
        if(circle)
        {
          lua_newtable(L);
          lua_pushnumber(L, circle->center[0]); lua_setfield(L, -2, "x");
          lua_pushnumber(L, circle->center[1]); lua_setfield(L, -2, "y");
          lua_setfield(L, -2, "target");
          lua_pushnumber(L, circle->radius); lua_setfield(L, -2, "radius");
          lua_pushnumber(L, circle->border); lua_setfield(L, -2, "feather");

          double dx = 0.0, dy = 0.0, dlen = 0.0;
          if(have_display
             && _mask_point_to_display(dev, wd, ht, iwidth, iheight,
                                       circle->center[0], circle->center[1], &dx, &dy))
          {
            lua_newtable(L);
            lua_pushnumber(L, dx); lua_setfield(L, -2, "x");
            lua_pushnumber(L, dy); lua_setfield(L, -2, "y");
            lua_setfield(L, -2, "target_display");

            if(_mask_len_to_display(dev, wd, ht, iwidth, iheight,
                                    circle->center[0], circle->center[1],
                                    circle->radius, &dlen))
            {
              lua_pushnumber(L, dlen); lua_setfield(L, -2, "radius_display");
            }
            if(_mask_len_to_display(dev, wd, ht, iwidth, iheight,
                                    circle->center[0], circle->center[1],
                                    circle->border, &dlen))
            {
              lua_pushnumber(L, dlen); lua_setfield(L, -2, "feather_display");
            }
          }
        }
      }
      else
      {
        // pre-existing GUI-created ellipse/path/brush shape -- not creatable
        // via MCP yet (Phase 2), but don't crash reading it back.
        lua_pushstring(L, "unknown"); lua_setfield(L, -2, "shape_type");
      }

      if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
      {
        lua_newtable(L);
        lua_pushnumber(L, form->source[0]); lua_setfield(L, -2, "x");
        lua_pushnumber(L, form->source[1]); lua_setfield(L, -2, "y");
        lua_setfield(L, -2, "source");

        double sx = 0.0, sy = 0.0;
        if(have_display
           && _mask_point_to_display(dev, wd, ht, iwidth, iheight,
                                     form->source[0], form->source[1], &sx, &sy))
        {
          lua_newtable(L);
          lua_pushnumber(L, sx); lua_setfield(L, -2, "x");
          lua_pushnumber(L, sy); lua_setfield(L, -2, "y");
          lua_setfield(L, -2, "source_display");
        }
      }

      lua_rawseti(L, -2, out_index++);
    }
  }

  lua_setfield(L, -2, "shapes");
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

/* darktable.develop.get_blend_params(op, instance)
   ------------------------------------------------------------------
   LUT tooling (2026-07-26): lut3d has no "amount" of its own -- the ONLY way
   to run a LUT at partial strength (the reported 15-50% portrait use case)
   is the module's blend opacity. blend_params is a SEPARATE flat struct
   (dt_develop_blend_params_t, develop/blend.h) from the introspected
   per-module params get_params()/set_params() already expose, so those two
   generic bindings never touch it -- hence this dedicated pair.

   opacity here is 0..100 (blend.h's own native scale, see blend.c:321 et al
   CLIP(bp->opacity / 100.0f)), matching the EXISTING set_raster_source_cb
   convention above, not mask_object/retouch's 0..1 -- do not conflate the
   two; the Python layer converts at its boundary (same pattern already
   documented for mask_raster's opacity, see CLAUDE.md).

   Returns {op, instance, opacity, mask_mode, blend_mode} or {error=...} if
   no darkroom image, no such module/instance, or the module does not
   support blending at all (module->flags() lacks IOP_FLAGS_SUPPORTS_BLENDING
   -- a handful of modules, e.g. rawdenoise's precursor stages, opt out).
   Read-only, but reads live develop state -> GUI thread (gtk_wrap). */
static int get_blend_params_cb(lua_State *L)
{
  const char *op = luaL_checkstring(L, 1);
  const int instance = luaL_optinteger(L, 2, 0);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  dt_iop_module_t *module = _find_module(dev, op, instance);
  if(!module)
    return push_error(L, "no module '%s' instance %d in current darkroom", op, instance);
  if(!module->blend_params || !module->flags || !(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING))
    return push_error(L, "module '%s' does not support blending", op);

  lua_newtable(L);
  lua_pushstring(L, module->op);                    lua_setfield(L, -2, "op");
  lua_pushinteger(L, module->multi_priority);       lua_setfield(L, -2, "instance");
  lua_pushnumber(L, module->blend_params->opacity); lua_setfield(L, -2, "opacity");
  lua_pushinteger(L, (lua_Integer)module->blend_params->mask_mode);
  lua_setfield(L, -2, "mask_mode");
  lua_pushinteger(L, (lua_Integer)module->blend_params->blend_mode);
  lua_setfield(L, -2, "blend_mode");
  lua_pushboolean(L, (module->blend_params->mask_combine & DEVELOP_COMBINE_MASKS_POS) ? TRUE : FALSE);
  lua_setfield(L, -2, "invert");
  return 1;
}

/* darktable.develop.set_blend_params(op, instance, {opacity?, blend_mode?,
   enable_uniform_blend?, invert?})
   ------------------------------------------------------------------
   Write blend_params fields directly (plain struct, no introspection --
   see get_blend_params_cb above for why this is a separate pair from
   get_params/set_params) and commit via dt_dev_add_history_item, exactly
   the write+commit shape set_raster_source_cb above already uses for this
   same struct.

   enable_uniform_blend, if given, OVERWRITES mask_mode wholesale: true ->
   DEVELOP_MASK_ENABLED (blend.h's own "uniformly" mode -- the whole module
   output blended toward the base image at `opacity`, no mask), false ->
   DEVELOP_MASK_DISABLED (module runs at its native 100%, opacity ignored).
   This is deliberately all-or-nothing: a caller wanting a MASKED blend
   (drawn/parametric/raster) already has retouch_add_shape, mask_object,
   mask_raster and set_raster_source for that, each of which sets mask_mode's other bits
   itself -- mixing this convenience in would risk silently clobbering a
   mask_id/raster wiring those tools set up. opacity alone, with mask_mode
   left untouched, is a no-op on a module whose blend was never enabled
   (mask_mode defaults to DEVELOP_MASK_DISABLED = 0) -- pass
   enable_uniform_blend=true alongside it the first time.

   invert toggles DEVELOP_COMBINE_MASKS_POS in blend_params->mask_combine --
   the exact bit the GUI's mask "polarity" toggle flips (_blendop_masks_
   polarity_callback, develop/blend_gui.c:1735), i.e. inverts the WHOLE drawn/
   parametric/raster mask for this module. Per-shape polarity is a SEPARATE
   bit (DT_MASKS_STATE_INVERSE on the individual dt_masks_point_group_t,
   reported read-only by list_masks' `invert` field) that this binding does
   not write -- attach_mask does not expose it either (not asked for; add it
   there if a caller ever needs per-shape rather than whole-group polarity).

   Returns {ok=true, op, instance, opacity, mask_mode, blend_mode, invert} (the
   POST-write values) or {error=...}. opacity is clamped to [0,100], never
   rejected -- no clamp report table, unlike set_params, since blend_params
   has no introspection Min/Max to report against. */
static int set_blend_params_cb(lua_State *L)
{
  const char *op = luaL_checkstring(L, 1);
  const int instance = luaL_optinteger(L, 2, 0);
  luaL_checktype(L, 3, LUA_TTABLE);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  dt_iop_module_t *module = _find_module(dev, op, instance);
  if(!module)
    return push_error(L, "no module '%s' instance %d in current darkroom", op, instance);
  if(!module->blend_params || !module->flags || !(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING))
    return push_error(L, "module '%s' does not support blending", op);

  lua_getfield(L, 3, "enable_uniform_blend");
  if(!lua_isnil(L, -1))
  {
    module->blend_params->mask_mode = lua_toboolean(L, -1) ? DEVELOP_MASK_ENABLED : DEVELOP_MASK_DISABLED;
  }
  lua_pop(L, 1);

  lua_getfield(L, 3, "opacity");
  if(!lua_isnil(L, -1))
  {
    double opacity = lua_tonumber(L, -1);
    if(opacity < 0.0) opacity = 0.0;
    if(opacity > 100.0) opacity = 100.0;
    module->blend_params->opacity = (float)opacity;
  }
  lua_pop(L, 1);

  lua_getfield(L, 3, "blend_mode");
  if(!lua_isnil(L, -1))
  {
    module->blend_params->blend_mode = (uint32_t)lua_tointeger(L, -1);
  }
  lua_pop(L, 1);

  lua_getfield(L, 3, "invert");
  if(!lua_isnil(L, -1))
  {
    if(lua_toboolean(L, -1))
      module->blend_params->mask_combine |= DEVELOP_COMBINE_MASKS_POS;
    else
      module->blend_params->mask_combine &= ~DEVELOP_COMBINE_MASKS_POS;
  }
  lua_pop(L, 1);

  dt_dev_add_history_item(dev, module, module->enabled);
  if(module->expander) dt_iop_gui_update(module);

  lua_newtable(L);
  lua_pushboolean(L, TRUE);                         lua_setfield(L, -2, "ok");
  lua_pushstring(L, module->op);                     lua_setfield(L, -2, "op");
  lua_pushinteger(L, module->multi_priority);        lua_setfield(L, -2, "instance");
  lua_pushnumber(L, module->blend_params->opacity);  lua_setfield(L, -2, "opacity");
  lua_pushinteger(L, (lua_Integer)module->blend_params->mask_mode);
  lua_setfield(L, -2, "mask_mode");
  lua_pushinteger(L, (lua_Integer)module->blend_params->blend_mode);
  lua_setfield(L, -2, "blend_mode");
  lua_pushboolean(L, (module->blend_params->mask_combine & DEVELOP_COMBINE_MASKS_POS) ? TRUE : FALSE);
  lua_setfield(L, -2, "invert");
  return 1;
}

/* darktable.develop.current_image()
   Read-only: report the image currently open in darkroom -- its imgid and
   absolute on-disk path -- so a caller can resolve the source file WITHOUT
   having had to open it via open_darkroom (the common case: the user opened a
   photo BY HAND in the GUI, so the MCP side never cached the path). Returns
   {has_image, id, path, filename}; has_image=false (id=-1) when no darkroom
   image is loaded. Mirrors the old DBus get_current_image. Reads live develop
   state -> registered wrapped in dt_lua_gtk_wrap. */
static int current_image_cb(lua_State *L)
{
  dt_develop_t *dev = darktable.develop;
  lua_newtable(L);

  const dt_imgid_t imgid = dev ? dev->image_storage.id : NO_IMGID;
  if(!dev || !dt_is_valid_imgid(imgid))
  {
    lua_pushboolean(L, FALSE); lua_setfield(L, -2, "has_image");
    lua_pushinteger(L, -1);    lua_setfield(L, -2, "id");
    return 1;
  }

  char pathname[PATH_MAX] = { 0 };
  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, pathname, sizeof(pathname), &from_cache);

  lua_pushboolean(L, TRUE);                        lua_setfield(L, -2, "has_image");
  lua_pushinteger(L, (lua_Integer)imgid);          lua_setfield(L, -2, "id");
  lua_pushstring(L, pathname);                     lua_setfield(L, -2, "path");
  lua_pushstring(L, dev->image_storage.filename);  lua_setfield(L, -2, "filename");
  return 1;
}

/* Human-readable mask type name for a dt_masks_type_t bitmask. */
static const char *_mask_type_name(dt_masks_type_t t)
{
  if(t & DT_MASKS_CIRCLE)   return "circle";
  if(t & DT_MASKS_PATH)     return "path";
  if(t & DT_MASKS_GROUP)    return "group";
  if(t & DT_MASKS_GRADIENT) return "gradient";
  if(t & DT_MASKS_ELLIPSE)  return "ellipse";
  if(t & DT_MASKS_BRUSH)    return "brush";
  return "unknown";
}

/* darktable.develop.list_all_masks()
   Read-only: enumerate EVERY drawn mask shape in the current image
   (dev->forms), regardless of which module currently uses it -- so a caller
   can find a shape drawn by hand in the GUI (or by another module's tool
   earlier in the session) and wire it into a brand new module instance via
   attach_mask(), without guessing or duplicating it. Skips DT_MASKS_GROUP
   forms themselves: those are the per-module blend-group CONTAINERS
   (module->blend_params->mask_id -> one dt_masks_form_t of type
   DT_MASKS_GROUP per module, see dt_masks_group_ensure_for_module), internal
   plumbing rather than a user-facing shape.

   Each entry: {formid, name, type, used_by=[{op, instance}, ...]}. used_by is
   resolved by walking dev->iop and checking every module's blend group for a
   point referencing this formid -- a shape can legally be referenced by more
   than one module's group at once (that is the whole point of attach_mask not
   copying the shape, dt_masks_group_add_form, masks.c:2084).

   Reads live dev->forms + dev->iop -> registered wrapped in dt_lua_gtk_wrap. */
static int list_all_masks_cb(lua_State *L)
{
  dt_develop_t *dev = darktable.develop;
  if(!dev) return push_error(L, "no darkroom image loaded");

  lua_newtable(L);
  int idx = 1;
  for(GList *forms = dev->forms; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *f = (dt_masks_form_t *)forms->data;
    if(!f || (f->type & DT_MASKS_GROUP)) continue; // skip per-module group containers

    lua_newtable(L);
    lua_pushinteger(L, f->formid);               lua_setfield(L, -2, "formid");
    lua_pushstring(L, f->name);                   lua_setfield(L, -2, "name");
    lua_pushstring(L, _mask_type_name(f->type));  lua_setfield(L, -2, "type");

    lua_newtable(L); // used_by[]
    int uidx = 1;
    for(GList *iops = dev->iop; iops; iops = g_list_next(iops))
    {
      dt_iop_module_t *m = (dt_iop_module_t *)iops->data;
      if(!m || !m->blend_params) continue;
      dt_masks_form_t *grp = dt_masks_get_from_id(dev, m->blend_params->mask_id);
      if(!grp || !(grp->type & DT_MASKS_GROUP)) continue;

      gboolean found = FALSE;
      for(GList *pts = grp->points; pts; pts = g_list_next(pts))
      {
        dt_masks_point_group_t *pg = (dt_masks_point_group_t *)pts->data;
        if(pg && pg->formid == f->formid) { found = TRUE; break; }
      }
      if(found)
      {
        lua_newtable(L);
        lua_pushstring(L, m->op);               lua_setfield(L, -2, "op");
        lua_pushinteger(L, m->multi_priority);   lua_setfield(L, -2, "instance");
        lua_rawseti(L, -2, uidx++);
      }
    }
    lua_setfield(L, -2, "used_by");

    lua_rawseti(L, -2, idx++);
  }
  return 1;
}

/* darktable.develop.attach_mask(op, instance, formid[, operation])
   Wire an EXISTING drawn mask shape (formid, from list_all_masks() or the
   return value of add_path_mask/mask_object/retouch_add_shape) into module
   (op, instance)'s blend group WITHOUT copying it -- the shape stays a single
   dev->forms entry, now referenced by this module's group IN ADDITION to
   whatever already referenced it. Reuses dt_masks_group_add_form (masks.c:
   2084), the exact function the GUI's "add existing shape" menu item calls
   (_menu_add_exist, masks.c:1638). Creates the module's blend group on first
   use (dt_masks_group_ensure_for_module) exactly like the GUI would.

   operation selects how this shape combines with the OTHER shapes already in
   the group (masks.h DT_MASKS_STATE_* bits): "union" (default),
   "intersection", "difference", "exclusion". Meaningless-but-harmless for the
   first shape added to an empty group (nothing to combine against yet).

   Wiring the group reference alone does NOT make the mask actually apply
   during pixel processing (bugreport 2026-07-27): dt_dev_pixelpipe's
   _piece_wants_blending (pixelpipe_hb.c:1383) gates the blend call on
   blend_params->mask_mode & DEVELOP_MASK_ENABLED, and blend.c's own
   mode_drawn check (blend.c:469) needs DEVELOP_MASK_MASK -- neither bit is
   implied by dt_masks_group_add_form. The GUI's own "enable drawn mask"
   pencil toggle (_blendop_masks_modes_toggle, blend_gui.c:1442) ORs exactly
   these two bits onto whatever mask_mode already had (so an existing
   parametric/raster mask stays enabled too); this binding replicates that
   OR (never a plain assignment, so other mask_mode bits already set by
   set_raster_source/mask_raster/mask_object survive untouched) so a module
   attached here actually shows/applies the mask without the user having to
   click the pencil icon manually. The result table's mask_mode is read back
   from module->blend_params AFTER the write (not the value we computed) so
   a caller can verify the bit really stuck rather than trusting the call
   succeeded silently.

   Mutates dev->forms / blend group / blend_params, commits history via
   dt_dev_add_masks_history_item -> MUST run on the GUI thread. */
static int attach_mask_cb(lua_State *L)
{
  const char *op = luaL_checkstring(L, 1);
  const int instance = (int)luaL_optinteger(L, 2, 0);
  const dt_mask_id_t formid = (dt_mask_id_t)luaL_checkinteger(L, 3);
  const char *operation = luaL_optstring(L, 4, "union");

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  dt_iop_module_t *module = _find_module(dev, op, instance);
  if(!module)
    return push_error(L, "no module '%s' instance %d in current darkroom", op, instance);
  if(!module->blend_params || !module->flags || !(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING))
    return push_error(L, "module '%s' does not support blending", op);

  dt_masks_form_t *form = dt_masks_get_from_id(dev, formid);
  if(!form)
    return push_error(L, "mask %d not found in darkroom forms", formid);
  if(form->type & DT_MASKS_GROUP)
    return push_error(L, "mask %d is a module blend group, not an attachable shape", formid);

  uint32_t state_bit;
  if(!g_strcmp0(operation, "union"))             state_bit = DT_MASKS_STATE_UNION;
  else if(!g_strcmp0(operation, "intersection")) state_bit = DT_MASKS_STATE_INTERSECTION;
  else if(!g_strcmp0(operation, "difference"))   state_bit = DT_MASKS_STATE_DIFFERENCE;
  else if(!g_strcmp0(operation, "exclusion"))    state_bit = DT_MASKS_STATE_EXCLUSION;
  else
    return push_error(L, "unknown operation '%s' (expected union/intersection/difference/exclusion)",
                      operation);

  dt_masks_form_t *grp = dt_masks_group_ensure_for_module(dev, module);
  if(!grp)
    return push_error(L, "could not create blend group for module '%s'", op);

  dt_masks_point_group_t *grpt = dt_masks_group_add_form(grp, form);
  if(!grpt)
    return push_error(L, "mask %d cannot be attached (would contain itself)", formid);
  grpt->state = DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW | state_bit;

  // Bugreport 2026-07-27: attaching a shape to the group is not enough --
  // the drawn-mask bits still need to be ON for the pipeline to use it (see
  // the long comment above). OR, never assign, so any bits set by
  // set_raster_source/mask_raster/mask_object (raster/conditional) survive.
  module->blend_params->mask_mode |= (DEVELOP_MASK_MASK | DEVELOP_MASK_ENABLED);

  dt_dev_add_masks_history_item(dev, module, TRUE);
  dt_masks_iop_update(module);
  dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);

  lua_newtable(L);
  lua_pushboolean(L, TRUE);                    lua_setfield(L, -2, "ok");
  lua_pushstring(L, module->op);               lua_setfield(L, -2, "op");
  lua_pushinteger(L, module->multi_priority);  lua_setfield(L, -2, "instance");
  lua_pushinteger(L, formid);                  lua_setfield(L, -2, "formid");
  lua_pushstring(L, operation);                lua_setfield(L, -2, "operation");
  // Read back from the live struct (not the value we just computed) so a
  // caller can verify the drawn-mask bit really stuck instead of trusting
  // the write silently.
  lua_pushinteger(L, (lua_Integer)module->blend_params->mask_mode);
  lua_setfield(L, -2, "mask_mode");
  return 1;
}

/* darktable.develop.detach_mask(op, instance, formid)
   Unwire a drawn mask shape from module (op, instance)'s blend group WITHOUT
   deleting the shape itself. dt_masks_form_remove(module, grp, form) with a
   non-NULL grp only drops the dt_masks_point_group_t link node (masks.c:
   1891-1922), the underlying form survives in dev->forms and can be
   re-attached to any module (this one or another) via attach_mask().

   If this was the LAST shape in the group, dt_masks_form_remove recurses into
   a full group deletion (masks.c:1920, grp->points == NULL) and resets
   blend_params->mask_id to NO_MASKID on every module that referenced that
   now-empty group (masks.c:1949-1953) -- expected, mirrors exactly what the
   GUI's own "no masks" action does (_menu_no_masks, masks.c:1611).

   dt_masks_form_remove commits history itself (dt_dev_add_masks_history_item,
   masks.c:1917, or dt_dev_add_history_item on the full-delete path) -> this
   entire call MUST run on the GUI thread. */
static int detach_mask_cb(lua_State *L)
{
  const char *op = luaL_checkstring(L, 1);
  const int instance = (int)luaL_optinteger(L, 2, 0);
  const dt_mask_id_t formid = (dt_mask_id_t)luaL_checkinteger(L, 3);

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dev->iop)
    return push_error(L, "no darkroom image loaded");

  dt_iop_module_t *module = _find_module(dev, op, instance);
  if(!module)
    return push_error(L, "no module '%s' instance %d in current darkroom", op, instance);
  if(!module->blend_params)
    return push_error(L, "module '%s' has no blend params", op);

  dt_masks_form_t *grp = dt_masks_get_from_id(dev, module->blend_params->mask_id);
  if(!grp || !(grp->type & DT_MASKS_GROUP))
    return push_error(L, "module '%s' instance %d has no shapes group", op, instance);

  dt_masks_form_t *form = dt_masks_get_from_id(dev, formid);
  if(!form)
    return push_error(L, "mask %d not found in darkroom forms", formid);

  gboolean present = FALSE;
  for(GList *pts = grp->points; pts; pts = g_list_next(pts))
  {
    dt_masks_point_group_t *pg = (dt_masks_point_group_t *)pts->data;
    if(pg && pg->formid == formid) { present = TRUE; break; }
  }
  if(!present)
    return push_error(L, "mask %d is not attached to module '%s' instance %d", formid, op, instance);

  dt_masks_form_remove(module, grp, form);

  lua_newtable(L);
  lua_pushboolean(L, TRUE);                    lua_setfield(L, -2, "ok");
  lua_pushstring(L, module->op);               lua_setfield(L, -2, "op");
  lua_pushinteger(L, module->multi_priority);  lua_setfield(L, -2, "instance");
  lua_pushinteger(L, formid);                  lua_setfield(L, -2, "formid");
  return 1;
}

/* Push {x=.., y=..} from a float[2] under `key` of the table at top of stack. */
static void _push_xy(lua_State *L, const char *key, const float xy[2])
{
  lua_newtable(L);
  lua_pushnumber(L, xy[0]); lua_setfield(L, -2, "x");
  lua_pushnumber(L, xy[1]); lua_setfield(L, -2, "y");
  lua_setfield(L, -2, key);
}

/* Map a dt_masks_point_group_t's state bits to the boolean-combine operation
   name (masks.h DT_MASKS_STATE_* bits) so callers can see how each shape
   combines with the OTHERS already in the group, and pick the same set of
   names attach_mask()/set_module_mask() accept. Falls back to "union" (the
   default dt_masks_group_add_form assigns, masks.c:2098) when no explicit
   operator bit is set -- true for the first shape added to an empty group. */
static const char *_mask_state_operation_name(int state)
{
  if(state & DT_MASKS_STATE_INTERSECTION) return "intersection";
  if(state & DT_MASKS_STATE_DIFFERENCE)   return "difference";
  if(state & DT_MASKS_STATE_EXCLUSION)    return "exclusion";
  return "union";
}

/* darktable.develop.list_masks(op, instance)
   Read-only: enumerate the drawn masks wired into a module instance's blend
   group, so a caller knows EXACTLY which mask_id it is about to edit (and does
   not clobber a stale instance). Returns [] when the module has no drawn masks.
   Each entry: {mask_id, type, opacity, nb_points, name, module, instance,
   operation, invert}. operation/invert (added alongside attach_mask/
   detach_mask) report the boolean-combine bits already stored per shape --
   this is the same list a module's mask manager panel shows, just read-only
   and headless. Reads live dev->forms -> registered wrapped in dt_lua_gtk_wrap. */
static int list_masks_cb(lua_State *L)
{
  dt_develop_t *dev = darktable.develop;
  const char *op = luaL_checkstring(L, 1);
  const int instance = (int)luaL_optinteger(L, 2, 0);
  if(!dev) return push_error(L, "no darkroom image loaded");
  dt_iop_module_t *module = _find_module(dev, op, instance);
  if(!module) return push_error(L, "module '%s' instance %d not found", op, instance);
  if(!module->blend_params) return push_error(L, "module '%s' has no blend params", op);

  lua_newtable(L);
  dt_masks_form_t *grp = dt_masks_get_from_id(dev, module->blend_params->mask_id);
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return 1;  // empty: no drawn masks

  int idx = 1;
  for(GList *it = grp->points; it; it = g_list_next(it))
  {
    dt_masks_point_group_t *pg = (dt_masks_point_group_t *)it->data;
    if(!pg) continue;
    dt_masks_form_t *f = dt_masks_get_from_id(dev, pg->formid);
    if(!f) continue;
    lua_newtable(L);
    lua_pushinteger(L, pg->formid);                            lua_setfield(L, -2, "mask_id");
    lua_pushstring(L, _mask_type_name(f->type));               lua_setfield(L, -2, "type");
    lua_pushnumber(L, pg->opacity);                            lua_setfield(L, -2, "opacity");
    lua_pushinteger(L, (lua_Integer)g_list_length(f->points)); lua_setfield(L, -2, "nb_points");
    lua_pushstring(L, f->name);                                lua_setfield(L, -2, "name");
    lua_pushstring(L, op);                                     lua_setfield(L, -2, "module");
    lua_pushinteger(L, instance);                              lua_setfield(L, -2, "instance");
    lua_pushstring(L, _mask_state_operation_name(pg->state));  lua_setfield(L, -2, "operation");
    lua_pushboolean(L, (pg->state & DT_MASKS_STATE_INVERSE) ? TRUE : FALSE);
    lua_setfield(L, -2, "invert");
    lua_rawseti(L, -2, idx++);
  }
  return 1;
}

/* darktable.develop.get_mask(mask_id)
   Read-only: dump one mask form's geometry so a caller can edit individual
   points instead of recreating the whole mask. Returns {mask_id, type, name,
   points=[...]} where each node carries its control handles + per-node border
   (feather). Path/brush -> node list; circle/ellipse/gradient -> single-entry
   shape descriptor. Reads live dev->forms -> registered wrapped in
   dt_lua_gtk_wrap. */
static int get_mask_cb(lua_State *L)
{
  dt_develop_t *dev = darktable.develop;
  const dt_mask_id_t id = (dt_mask_id_t)luaL_checkinteger(L, 1);
  if(!dev) return push_error(L, "no darkroom image loaded");
  dt_masks_form_t *f = dt_masks_get_from_id(dev, id);
  if(!f) return push_error(L, "mask %d not found in darkroom forms", id);

  lua_newtable(L);
  lua_pushinteger(L, f->formid);                lua_setfield(L, -2, "mask_id");
  lua_pushstring(L, _mask_type_name(f->type));  lua_setfield(L, -2, "type");
  lua_pushstring(L, f->name);                   lua_setfield(L, -2, "name");

  lua_newtable(L);  // points[]
  int idx = 1;
  if(f->type & DT_MASKS_PATH)
  {
    for(GList *it = f->points; it; it = g_list_next(it))
    {
      dt_masks_point_path_t *p = (dt_masks_point_path_t *)it->data;
      if(!p) continue;
      lua_newtable(L);
      _push_xy(L, "corner", p->corner);
      _push_xy(L, "ctrl1", p->ctrl1);
      _push_xy(L, "ctrl2", p->ctrl2);
      _push_xy(L, "border", p->border);
      lua_pushinteger(L, p->state); lua_setfield(L, -2, "state");
      lua_rawseti(L, -2, idx++);
    }
  }
  else if(f->type & DT_MASKS_BRUSH)
  {
    for(GList *it = f->points; it; it = g_list_next(it))
    {
      dt_masks_point_brush_t *p = (dt_masks_point_brush_t *)it->data;
      if(!p) continue;
      lua_newtable(L);
      _push_xy(L, "corner", p->corner);
      _push_xy(L, "ctrl1", p->ctrl1);
      _push_xy(L, "ctrl2", p->ctrl2);
      _push_xy(L, "border", p->border);
      lua_pushnumber(L, p->density);  lua_setfield(L, -2, "density");
      lua_pushnumber(L, p->hardness); lua_setfield(L, -2, "hardness");
      lua_pushinteger(L, p->state);   lua_setfield(L, -2, "state");
      lua_rawseti(L, -2, idx++);
    }
  }
  else if(f->type & DT_MASKS_CIRCLE)
  {
    dt_masks_point_circle_t *c = f->points ? (dt_masks_point_circle_t *)f->points->data : NULL;
    if(c)
    {
      lua_newtable(L);
      _push_xy(L, "center", c->center);
      lua_pushnumber(L, c->radius); lua_setfield(L, -2, "radius");
      lua_pushnumber(L, c->border); lua_setfield(L, -2, "border");
      lua_rawseti(L, -2, idx++);
    }
  }
  else if(f->type & DT_MASKS_ELLIPSE)
  {
    dt_masks_point_ellipse_t *e = f->points ? (dt_masks_point_ellipse_t *)f->points->data : NULL;
    if(e)
    {
      lua_newtable(L);
      _push_xy(L, "center", e->center);
      lua_pushnumber(L, e->radius[0]); lua_setfield(L, -2, "radius_a");
      lua_pushnumber(L, e->radius[1]); lua_setfield(L, -2, "radius_b");
      lua_pushnumber(L, e->rotation);  lua_setfield(L, -2, "rotation");
      lua_pushnumber(L, e->border);    lua_setfield(L, -2, "border");
      lua_rawseti(L, -2, idx++);
    }
  }
  lua_setfield(L, -2, "points");
  return 1;
}

/* darktable.develop.rename_mask(mask_id, name)
   Give a drawn mask a caller-chosen name instead of the auto-generated
   "path #7"/"circle #3" -- bugreport 2026-07-31: an agent creating many
   masks across a session (e.g. several mask_object/add_path_mask calls on
   one image) has no way to tell them apart afterward except by formid,
   which hurts orientation. Mirrors the masks-manager panel's own rename
   text-entry callback EXACTLY (src/libs/masks.c's _tree_name_edited,
   g_strlcpy + dt_dev_add_masks_history_item(dev, NULL, FALSE) -- module
   NULL resolves internally to the mask_manager module, same as the GUI's
   own headless-safe path).

   Empty name is coerced to a single space (same guard as the GUI): an
   empty form->name serializes into the XMP sidecar as a bare "<rdf:li/>"
   element, which is a known-bad edge case the GUI already works around
   this same way, not something new introduced here.

   Returns {ok, mask_id, name} with `name` read back from the live struct
   (not the requested string) -- same verify-after-write discipline as
   attach_mask's mask_mode readback, so a caller never has to trust a
   write silently. Mutates dev->forms, commits history -> GUI thread. */
static int rename_mask_cb(lua_State *L)
{
  dt_develop_t *dev = darktable.develop;
  const dt_mask_id_t id = (dt_mask_id_t)luaL_checkinteger(L, 1);
  const char *name = luaL_checkstring(L, 2);
  if(!dev) return push_error(L, "no darkroom image loaded");
  dt_masks_form_t *f = dt_masks_get_from_id(dev, id);
  if(!f) return push_error(L, "mask %d not found in darkroom forms", id);

  const char *safe_name = (name[0] == '\0') ? " " : name;
  g_strlcpy(f->name, safe_name, sizeof(f->name));
  dt_dev_add_masks_history_item(dev, NULL, FALSE);

  lua_newtable(L);
  lua_pushboolean(L, TRUE);       lua_setfield(L, -2, "ok");
  lua_pushinteger(L, f->formid);  lua_setfield(L, -2, "mask_id");
  lua_pushstring(L, f->name);     lua_setfield(L, -2, "name");
  return 1;
}

/* darktable.develop.delete_mask(mask_id)
   Permanently delete a drawn mask shape, regardless of whether it is
   currently attached to any module's blend group (bugreport 2026-07-31:
   no way to clean up an orphan/unwanted mask left over from calibration/
   experimentation -- detach_mask only unwires it, still leaving it in
   dev->forms forever). Thin wrapper around dt_masks_form_remove(NULL,
   NULL, form) with grp=NULL -- the exact "permanently delete this form"
   code path masks.c's own detach_mask cascade ALREADY exercises when a
   group's last shape is detached (masks.c:1901 onward: sweeps every
   module referencing the shape, clears mask_id where it was the base
   group, commits history per affected module) -- reused as-is, module=
   NULL is fine since that branch never dereferences its module argument
   for a form-wide delete (only the earlier "detach from THIS ONE group"
   branch does, which grp=NULL skips entirely). Returns {ok, mask_id} --
   errors if the mask_id doesn't exist (nothing silently no-oped). */
static int delete_mask_cb(lua_State *L)
{
  dt_develop_t *dev = darktable.develop;
  const dt_mask_id_t id = (dt_mask_id_t)luaL_checkinteger(L, 1);
  if(!dev) return push_error(L, "no darkroom image loaded");
  dt_masks_form_t *f = dt_masks_get_from_id(dev, id);
  if(!f) return push_error(L, "mask %d not found in darkroom forms", id);

  dt_masks_form_remove(NULL, NULL, f);

  lua_newtable(L);
  lua_pushboolean(L, TRUE);  lua_setfield(L, -2, "ok");
  lua_pushinteger(L, id);    lua_setfield(L, -2, "mask_id");
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

  // get_conf_string(key): dt_conf_get_string() has its own mutex, no
  // GUI-thread marshaling needed (see get_conf_string_cb comment).
  lua_pushcfunction(L, get_conf_string_cb);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "get_conf_string");

  // active_modules(): reads live develop state -> wrap onto the GUI thread.
  lua_pushcfunction(L, active_modules_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "active_modules");

  // current_image(): read-only report of the darkroom image (id + full path) ->
  // GUI thread. Lets mask_raster resolve the file for a hand-opened image.
  lua_pushcfunction(L, current_image_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "current_image");

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

  // add_instance(op[, fields]): duplicates a module via the GUI new-instance
  // path (dt_iop_gui_duplicate), optionally applying initial param overrides
  // (fields) via a nested set_params_cb call. Writes live develop/GUI state
  // -> MUST run on the GUI thread.
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

  // retouch_add_shape/delete_shape/list_shapes: local heal/clone retouch
  // shapes tied to the retouch module's own rt_forms[] + wavelet scale.
  // Mutate dev->forms / module params / history -> MUST run on the GUI
  // thread (see the long note at the top of retouch_add_shape_cb).
  lua_pushcfunction(L, retouch_add_shape_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "retouch_add_shape");

  lua_pushcfunction(L, retouch_update_shape_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "retouch_update_shape");

  lua_pushcfunction(L, retouch_delete_shape_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "retouch_delete_shape");

  lua_pushcfunction(L, retouch_list_shapes_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "retouch_list_shapes");

  // set_raster_source(consumer_op, consumer_instance, source_op, source_instance
  // [, opacity]): SPIKE T3.1 -- wire a downstream module's blend to consume an
  // upstream module's RASTER mask. Mutates blend_params + history -> GUI thread.
  lua_pushcfunction(L, set_raster_source_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "set_raster_source");

  // get_blend_params(op, instance): reads live blend_params -> GUI thread.
  lua_pushcfunction(L, get_blend_params_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "get_blend_params");

  // set_blend_params(op, instance, {..}): writes blend_params + commits
  // history via dt_dev_add_history_item -> MUST run on the GUI thread.
  lua_pushcfunction(L, set_blend_params_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "set_blend_params");

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

  // set_viewport(viewport, zoom_x, zoom_y, scale[, wait_for_pipe]
  // [, timeout_ms]): writable zoom/pan. Registered DELIBERATELY WITHOUT
  // dt_lua_gtk_wrap -- exactly like preview() below, the freshness wait must
  // run on the Lua thread, not the GUI main loop (see the long threading
  // note above _viewport_op_apply_gui).
  lua_pushcfunction(L, set_viewport_cb);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "set_viewport");

  // restore_viewport(viewport, zoom, closeup, zoom_x, zoom_y, scale
  // [, wait_for_pipe][, timeout_ms]): put a previously-snapshotted zoom/pan
  // state back exactly. Same not-gtk-wrapped reasoning as set_viewport.
  lua_pushcfunction(L, restore_viewport_cb);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "restore_viewport");

  // backtransform_point(x, y[, len1][, len2]): processed/display-frame
  // normalized point -> pipe-input/mask-frame normalized point (the
  // conversion get_viewport()/get_preview()-derived coordinates need before
  // being handed to retouch_add_shape/retouch_update_shape/add_path_mask --
  // see the long note at the top of backtransform_point_cb). Reads live
  // dev->preview_pipe/history_mutex -> GUI thread.
  lua_pushcfunction(L, backtransform_point_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "backtransform_point");

  // transform_point(x, y[, len1][, len2]): the INVERSE direction -- pipe-input/
  // mask-frame normalized point -> processed/display-frame normalized point.
  // Needed to VERIFY or DRAW an existing mask shape against what
  // get_preview()/capture_viewport actually renders (retouch_list_shapes
  // reports mask-frame storage values; those two frames differ for every
  // portrait/cropped image). Reads live pipe state -> GUI thread.
  lua_pushcfunction(L, transform_point_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "transform_point");

  // list_masks(op, instance): read-only enumeration of a module's drawn masks
  // (mask_id/type/opacity/nb_points) so a caller edits the right mask. Reads
  // live dev->forms -> GUI thread.
  lua_pushcfunction(L, list_masks_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "list_masks");

  // get_mask(mask_id): read-only dump of one form's points + control handles +
  // per-node border (feather) so points can be edited in place. Reads live
  // dev->forms -> GUI thread.
  lua_pushcfunction(L, get_mask_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "get_mask");

  // rename_mask(mask_id, name): give a drawn mask a caller-chosen name
  // (2026-07-31 bugreport -- orientation among many masks). Mutates
  // dev->forms, commits history -> GUI thread.
  lua_pushcfunction(L, rename_mask_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "rename_mask");

  // delete_mask(mask_id): permanently delete a drawn mask shape (2026-07-31
  // bugreport -- no way to clean up an orphan mask). Mutates dev->forms,
  // commits history per affected module -> GUI thread.
  lua_pushcfunction(L, delete_mask_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "delete_mask");

  // list_all_masks(): read-only enumeration of EVERY drawn shape in the
  // current image (dev->forms), independent of which module uses it, plus
  // used_by (op/instance list per shape). Reads live dev->forms/dev->iop ->
  // GUI thread.
  lua_pushcfunction(L, list_all_masks_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "list_all_masks");

  // attach_mask(op, instance, formid[, operation]): wire an EXISTING shape
  // into a module's blend group without copying it. Mutates dev->forms/blend
  // group/blend_params/history -> MUST run on the GUI thread.
  lua_pushcfunction(L, attach_mask_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "attach_mask");

  // detach_mask(op, instance, formid): unwire a shape from a module's blend
  // group WITHOUT deleting it. Mutates blend group/blend_params/history ->
  // MUST run on the GUI thread.
  lua_pushcfunction(L, detach_mask_cb);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "detach_mask");

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
