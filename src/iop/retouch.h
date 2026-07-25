/*
    This file is part of darktable,
    Copyright (C) 2017-2026 darktable developers.

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
#pragma once

// Params struct extracted from retouch.c (agentic-mcp fork) so the Lua
// bridge (src/lua/develop.c) can build/edit/delete individual retouch
// shapes (rt_forms entries) directly, without duplicating the layout.
// darktable's build-time introspection parser (tools/introspection/) follows
// #include directives when resolving struct definitions and their
// $MIN/$MAX/$DEFAULT annotations, so moving this here from retouch.c is
// safe -- keep every annotation comment intact.

#include "common/darktable.h"

#define RETOUCH_NO_FORMS 300
#define RETOUCH_MAX_SCALES 15
#define RETOUCH_NO_SCALES (RETOUCH_MAX_SCALES + 2)

typedef enum dt_iop_retouch_fill_modes_t {
  DT_IOP_RETOUCH_FILL_ERASE = 0, // $DESCRIPTION: "erase"
  DT_IOP_RETOUCH_FILL_COLOR = 1  // $DESCRIPTION: "color"
} dt_iop_retouch_fill_modes_t;

typedef enum dt_iop_retouch_blur_types_t {
  DT_IOP_RETOUCH_BLUR_GAUSSIAN = 0, // $DESCRIPTION: "gaussian"
  DT_IOP_RETOUCH_BLUR_BILATERAL = 1 // $DESCRIPTION: "bilateral"
} dt_iop_retouch_blur_types_t;

typedef enum dt_iop_retouch_algo_type_t {
  DT_IOP_RETOUCH_NONE = 0,  // $DESCRIPTION: "unused"
  DT_IOP_RETOUCH_CLONE = 1, // $DESCRIPTION: "clone"
  DT_IOP_RETOUCH_HEAL = 2,  // $DESCRIPTION: "heal"
  DT_IOP_RETOUCH_BLUR = 3,  // $DESCRIPTION: "blur"
  DT_IOP_RETOUCH_FILL = 4   // $DESCRIPTION: "fill"
} dt_iop_retouch_algo_type_t;

typedef struct dt_iop_retouch_form_data_t
{
  dt_mask_id_t formid; // from masks, form->formid
  int scale;  // 0==original image; 1..RETOUCH_MAX_SCALES==scale; RETOUCH_MAX_SCALES+1==residual
  dt_iop_retouch_algo_type_t algorithm;  // clone, heal, blur, fill

  dt_iop_retouch_blur_types_t blur_type; // gaussian, bilateral
  float blur_radius;                     // radius for blur algorithm

  dt_iop_retouch_fill_modes_t fill_mode; // mode for fill algorithm, erase or fill with color
  float fill_color[3];                   // color for fill algorithm
  float fill_brightness;                 // value to be added to the color
  int distort_mode; // module v1 => 1, otherwise 2. mode 1 as issues if there's distortion before this module
} dt_iop_retouch_form_data_t;

typedef struct dt_iop_retouch_params_t
{
  dt_iop_retouch_form_data_t rt_forms[RETOUCH_NO_FORMS]; // array of masks index and additional data

  dt_iop_retouch_algo_type_t algorithm; // $DEFAULT: DT_IOP_RETOUCH_HEAL clone, heal, blur, fill

  int num_scales;       // $DEFAULT: 0 number of wavelets scales
  int curr_scale;       // $DEFAULT: 0 current wavelet scale
  int merge_from_scale; // $DEFAULT: 0

  float preview_levels[3];

  dt_iop_retouch_blur_types_t blur_type; // $DEFAULT: DT_IOP_RETOUCH_BLUR_GAUSSIAN $DESCRIPTION: "blur type" gaussian, bilateral
  float blur_radius; // $MIN: 0.1 $MAX: 200.0 $DEFAULT: 10.0 $DESCRIPTION: "blur radius" radius for blur algorithm

  dt_iop_retouch_fill_modes_t fill_mode; // $DEFAULT: DT_IOP_RETOUCH_FILL_ERASE $DESCRIPTION: "fill mode" mode for fill algorithm, erase or fill with color
  float fill_color[3];   // $DEFAULT: 0.0 color for fill algorithm
  float fill_brightness; // $MIN: -1.0 $MAX: 1.0 $DESCRIPTION: "brightness" value to be added to the color
  int max_heal_iter;     // $DEFAULT: 2000 $DESCRIPTION: "max_iter" number of iterations for heal algorithm
} dt_iop_retouch_params_t;
