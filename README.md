# darktable-agentic

A fork of [darktable-org/darktable](https://github.com/darktable-org/darktable) that adds a
`darktable.develop` Lua namespace: a C-level API for reading and writing darktable's **live**
darkroom editing state from outside the GUI.

> **This is not the darktable README.** For what darktable is, how to install it, supported
> platforms, dependencies, build instructions and the FAQ, go to
> **[darktable-org/darktable](https://github.com/darktable-org/darktable)**. This fork does not
> change the build system, so upstream build instructions apply verbatim.
>
> **This is also not the thing you install.** The MCP server, the Python bridge and the
> agent-facing tools live in a separate repo:
> **[rfordinal/darktable-mcp](https://github.com/rfordinal/darktable-mcp)**. Start there.

- Fork point: upstream `master` at `7d38b68` (2026-07-23, post-5.6.0)
- Default branch: `agentic-mcp`
- Diff against upstream: 6 files, ~3000 lines added (see [What this fork changes](#what-this-fork-changes))

## Why this fork exists

The goal is live conversational raw editing: a photographer says "brighten the sky a bit" or
"heal that dust spot", an LLM adjusts the actual darktable pixelpipe, and the photographer sees
the result in the darkroom preview immediately.

darktable's stock Lua API cannot do that. It exposes images, films, tags, styles, storages and
GUI actions, but not the live `dt_develop_t` state: no module params, no history, no masks, no
way to read back what the pipe just produced. The available workarounds are all dead ends for an
agent:

- `darktable-cli` never loads the user's library and cannot touch a live darkroom.
- `dt.gui.action` drives GUI widgets, which means single-image, focus-dependent, GUI-only, and it
  gives no readback of values or bounds.
- Writing XMP sidecars or poking `library.db` bypasses the running instance entirely, so the user
  sees nothing until a reload.

So the enabling work had to happen in C, in darktable itself. That is all this repo is: the
darktable-side patch the MCP layer depends on. No MCP protocol code, no Python, no agent logic
lives here.

## What this fork adds

A new Lua singleton, `darktable.develop`, implemented in
[`src/lua/develop.c`](src/lua/develop.c). Everything operates on **whichever image is currently
open in darkroom**, mutates live develop state, and commits through darktable's own
`dt_dev_add_history_item` path, so edits appear in the module GUI and in the history stack exactly
as if a human had made them. Nothing waits for a database round-trip.

### Handshake and lifecycle

| Call | Purpose |
| --- | --- |
| `version()` | `{api, dt_lua_api, min_bridge}`. The bridge refuses a mismatched API. Currently API **7**. |
| `current_image()` | `{has_image, id, path, filename}` for the darkroom image, so a caller can resolve an image the user opened by hand in the GUI. |
| `active_modules()` | Every module on the open image: `{op, instance, id, multi_name, enabled, has_introspection}`. |
| `enable_module(op, instance, on)` | Toggle a module and commit history. |
| `add_instance(op)` | Duplicate a module (multi-instance), same gate the GUI uses. |
| `history_count()` | `{count, history_end}`. Read-only, for observing history coalescing. |

`instance` always means `module->multi_priority`, the user-visible 0-based multi-instance index
(0 = base). The opaque internal history id is reported separately as `id`.

### Module parameters

| Call | Purpose |
| --- | --- |
| `get_params(op, instance)` | Decode a module's live params blob via darktable's introspection data. Numeric fields come back as `{value, min, max, default}`, bools bare, enums as `{value, label, options[]}`, and arrays/structs/unions are walked recursively (so tone curve nodes and similar composites are readable). |
| `set_params(op, instance, fields)` | Write fields, commit history, refresh the module widgets, reprocess. Returns `{ok, applied, clamped, unknown_fields}`. |
| `dump_introspection(op, instance)` | Read-only diagnostic dump of the raw introspection tree. |

`set_params` **clamps rather than rejects**: an out-of-range value is written at the bound and the
clamp is reported per field (`{field, requested, applied, min, max}`), so an agent can say "that
is already at maximum" instead of silently no-opping. Unknown field names are ignored and listed.
An enum value outside the declared set is reported and not written.

### Masks

| Call | Purpose |
| --- | --- |
| `add_path_mask(op, instance, points, opacity, feather, smooth)` | Build a `DT_MASKS_PATH` form from a normalized polygon, wire it into the module's blend group, and switch the module to drawn-mask blending so its effect is restricted to that region. Boundaries are smoothed with Catmull-Rom control points by default; `feather` is a fraction of the mask's own bounding box. |
| `list_masks(op, instance)` | Enumerate the drawn masks attached to a module instance: `{mask_id, type, opacity, nb_points, name, module, instance}`. |
| `get_mask(mask_id)` | Dump one form's geometry, including per-node control handles and per-node border, so individual points can be edited instead of recreating the mask. |
| `set_raster_source(consumer_op, consumer_instance, source_op, source_instance, opacity)` | Wire a downstream module's blend to consume an upstream module's **raster** mask, the same way the GUI raster-mask picker does. The source must be earlier in the pixelpipe. |

### Retouch (heal / clone)

| Call | Purpose |
| --- | --- |
| `retouch_add_shape(op, instance, algorithm, target_x, target_y, radius, feather, source_x, source_y, scale, opacity)` | Create a circle retouch shape (`"heal"` or `"clone"`) and bind it to a wavelet scale. |
| `retouch_delete_shape(op, instance, formid)` | Remove a shape and its `rt_forms[]` entry, compacting the array correctly without the GUI resync path. |
| `retouch_list_shapes(op, instance)` | The shapes actually attached, cross-referenced against `rt_forms[]` by formid, plus `{num_scales, curr_scale, merge_from_scale}`. Avoids exposing the raw 300-slot mostly-empty array that generic introspection would return. |

These write `rt_forms[]` in the retouch module's params directly rather than replaying GUI mouse
events, which is why the retouch params layout had to be moved into a public header (see below).

### Viewport and preview

| Call | Purpose |
| --- | --- |
| `preview(max_w, max_h, x, y, w, h)` | Grab the live `preview_pipe` backbuf, optionally crop a normalized sub-rectangle, downscale, and write a PNG under `$XDG_CACHE_HOME/darktable-mcp/develop-previews/`. **No database write.** Returns `{status="ok", path, width, height, frame_width, frame_height, region}`, or `status="processing"` with `stale_preview` if the fresh frame did not arrive in time. |
| `get_viewport()` | Read-only zoom/pan state of the darkroom canvas, for both the main window and the secondary (`preview2`) window. Each viewport carries a `region` `{x, y, w, h}`: the visible sub-rectangle of the image in top-left normalized coordinates, clamped to `[0,1]`, and a drop-in for `preview()`'s region argument. |

`preview()` is what closes the agent loop: change a param, look at the result, decide the next
nudge, all without committing anything to the library.

## Design notes

**Threading.** Live develop state and the history stack are only ever mutated on the GTK main
thread. Every binding except `version()` and `preview()` is registered wrapped in
`dt_lua_gtk_wrap`, so it is marshaled there. `preview()` is deliberately **not** wrapped: its
bounded wait for a fresh frame would otherwise block the GUI main loop it is waiting on, so the
wait runs on the Lua thread and only the small snapshot-and-trigger step is marshaled across.

**No crashes on bad input.** Every guard failure (not in darkroom, unknown module or instance, no
introspection data, degenerate geometry, one-instance-only module) returns an `{error = "..."}`
table. The bindings are called by a language model, so a hostile or confused argument has to be a
graceful error, never a segfault.

**Versioned wire shapes.** `DT_LUA_DEVELOP_API` is bumped whenever a return shape changes, and
`version()` exposes it so the Python bridge can refuse an incompatible darktable build instead of
misreading a table. The per-version changelog is a comment block at the top of `src/lua/develop.c`.

## What this fork changes

| File | Change |
| --- | --- |
| `src/lua/develop.c` | New. The whole `darktable.develop` namespace (~2900 lines). |
| `src/lua/develop.h` | New. Declares `dt_lua_init_develop`. |
| `src/lua/init.c` | Registers `dt_lua_init_develop` in the Lua init chain. |
| `src/CMakeLists.txt` | Adds `lua/develop.c` to the Lua source list. |
| `src/iop/retouch.h` | New. Moves `RETOUCH_NO_FORMS`, the algorithm/blur/fill enums, `dt_iop_retouch_form_data_t` and `dt_iop_retouch_params_t` out of `retouch.c` so `develop.c` can build and edit retouch shapes directly. |
| `src/iop/retouch.c` | Pure refactor: those definitions now come from the new header. No behavior change. |

Nothing else is touched. No pixel math, no GUI, no database schema, no build options.

## Building

Follow the [upstream build instructions](https://github.com/darktable-org/darktable#building)
unchanged, against this fork's `agentic-mcp` branch:

```bash
git clone --recurse-submodules -b agentic-mcp https://github.com/rfordinal/darktable-agentic.git
cd darktable-agentic
./build.sh --prefix /opt/darktable-agentic --build-type Release --install --sudo
```

Requirements beyond upstream: **Lua 5.4 must be enabled** (`USE_LUA`, on by default). Without it
the namespace is not compiled in.

Installing to a separate prefix and running with a separate config dir is recommended, so this
build does not share a database with your production darktable:

```bash
/opt/darktable-agentic/bin/darktable --configdir ~/.config/darktable-agentic
```

Verify the API is present:

```lua
-- in ~/.config/darktable-agentic/luarc
local dt = require "darktable"
dt.print_log("develop API: " .. tostring(dt.develop.version().api))
```

## Using it

You almost certainly do not want to call these bindings by hand. Install
**[rfordinal/darktable-mcp](https://github.com/rfordinal/darktable-mcp)**, which ships the Lua
plugin, the JSON bridge and the MCP tool surface an agent actually talks to, and which documents
the end-user tools (`get_params`, `set_params`, `get_preview`, retouch, contact sheet, export and
the rest). That repo's README is the installation and configuration guide; this one is the API
reference for the layer underneath.

For exact argument order, clamping ranges and return-table keys, read the per-function doc
comments in [`src/lua/develop.c`](src/lua/develop.c). They are the authoritative spec.

## Status

Experimental, and tracked against the MCP project's own phase plan. Broad shape of what is
solid and what is not:

- Scalar, bool and enum params, plus array/struct composites: working.
- Module enable and multi-instance: working.
- Live preview and viewport readback: working.
- Drawn path masks: working.
- Retouch circle heal/clone: Phase 1. Circle shapes only; no path/ellipse/brush retouch shapes,
  no blur or fill algorithms.
- Raster masks (`set_raster_source`): research spike, not hardened.
- Not upstreamable as-is. This is a product-driven patch set, not a proposed upstream feature.

## License

GPL-3.0-or-later, same as upstream darktable. All new files in this fork carry the darktable GPL
header. Note that the sibling [darktable-mcp](https://github.com/rfordinal/darktable-mcp) repo is
a fork of [w1ne/darktable-mcp](https://github.com/w1ne/darktable-mcp) and keeps its own upstream
license; only this repo is GPL3.

## Upstream

darktable is developed at [darktable-org/darktable](https://github.com/darktable-org/darktable)
by the darktable developers. All credit for darktable itself belongs there.

- [Homepage](https://www.darktable.org/)
- [User manual (dtdocs)](https://github.com/darktable-org/dtdocs)
- [Lua API docs (luadocs)](https://github.com/darktable-org/luadocs)
- [Community forum](https://discuss.pixls.us/c/software/darktable/19)
