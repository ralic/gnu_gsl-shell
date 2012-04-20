
/* lua-draw.cpp
 *
 * Copyright (C) 2009, 2010 Francesco Abbate
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <pthread.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include "lua-draw.h"
#include "lua-graph.h"
#include "text-shape.h"
#include "lua-cpp-utils.h"
#include "gs-types.h"
#include "trans.h"
#include "colors.h"
#include "sg_marker.h"

enum path_cmd_e {
  CMD_ERROR = -1,
  CMD_MOVE_TO = 0,
  CMD_LINE_TO,
  CMD_CLOSE,
  CMD_ARC_TO,
  CMD_CURVE3,
  CMD_CURVE4,
};

struct cmd_call_stack {
  double f[6];
  int b[2];
};

struct path_cmd_reg {
  enum path_cmd_e id;
  const char *cmd;
  const char *signature;
};

static int agg_path_free      (lua_State *L);
static int agg_path_index     (lua_State *L);

static int agg_ellipse_new    (lua_State *L);
static int agg_circle_new     (lua_State *L);
static int agg_ellipse_free   (lua_State *L);

static int textshape_new      (lua_State *L);
static int textshape_free     (lua_State *L);

static int marker_new         (lua_State *L);
static int marker_free        (lua_State *L);

static void path_cmd (draw::path *p, int cmd, struct cmd_call_stack *stack);

static struct path_cmd_reg cmd_table[] = {
  {CMD_MOVE_TO,  "move_to",  "ff"},
  {CMD_LINE_TO,  "line_to",  "ff"},
  {CMD_CLOSE,    "close",    ""},
  {CMD_ARC_TO,   "arc_to",   "fffbbff"},
  {CMD_CURVE3,   "curve3",   "ffff"},
  {CMD_CURVE4,   "curve4",   "ffffff"},
  {CMD_ERROR,    NULL, NULL}
};

static const struct luaL_Reg draw_functions[] = {
  {"path",     agg_path_new},
  {"ellipse",  agg_ellipse_new},
  {"circle",   agg_circle_new},
  {"textshape", textshape_new},
  {"marker",   marker_new},
  {NULL, NULL}
};

static const struct luaL_Reg agg_path_methods[] = {
  {"__index",     agg_path_index},
  {"__gc",        agg_path_free},
  {NULL, NULL}
};

static const struct luaL_Reg textshape_methods[] = {
  {"__gc",        textshape_free},
  {NULL, NULL}
};


static const struct luaL_Reg agg_ellipse_methods[] = {
  {"__gc",        agg_ellipse_free},
  {NULL, NULL}
};


static const struct luaL_Reg marker_methods[] = {
  {"__gc",        marker_free},
  {NULL, NULL}
};

int
agg_path_new (lua_State *L)
{
  draw::path *vs = push_new_object<draw::path>(L, GS_DRAW_PATH);

  if (lua_gettop (L) >= 2)
    {
      double x = gs_check_number (L, 1, FP_CHECK_NORMAL);
      double y = gs_check_number (L, 2, FP_CHECK_NORMAL);
      struct cmd_call_stack s[1];

      s->f[0] = x;
      s->f[1] = y;

      path_cmd (vs, CMD_MOVE_TO, s);
    }

  return 1;
}

draw::path *
check_agg_path (lua_State *L, int index)
{
  return (draw::path *) gs_check_userdata (L, index, GS_DRAW_PATH);
}

int
agg_path_free (lua_State *L)
{
  return object_free<draw::path>(L, 1, GS_DRAW_PATH);
}

void
path_cmd (draw::path *p, int _cmd, struct cmd_call_stack *s)
{
  agg::path_storage& ps = p->self();
  path_cmd_e cmd = (path_cmd_e) _cmd;

  switch (cmd)
    {
    case CMD_MOVE_TO:
      ps.move_to (s->f[0], s->f[1]);
      break;
    case CMD_LINE_TO:
      if (ps.total_vertices() == 0)
	ps.move_to (s->f[0], s->f[1]);
      else
	ps.line_to (s->f[0], s->f[1]);
      break;
    case CMD_CLOSE:
      ps.close_polygon ();
      break;
    case CMD_ARC_TO:
      ps.arc_to (s->f[0], s->f[1], s->f[2], s->b[0], s->b[1], s->f[3], s->f[4]);
      break;
    case CMD_CURVE3:
      ps.curve3 (s->f[0], s->f[1], s->f[2], s->f[3]);
      break;
    case CMD_CURVE4:
      ps.curve4 (s->f[0], s->f[1], s->f[2], s->f[3], s->f[4], s->f[5]);
      break;
    case CMD_ERROR:
      ;
    }
}

static int
agg_path_cmd (lua_State *L)
{
  draw::path *p = check_agg_path (L, 1);
  int id = lua_tointeger (L, lua_upvalueindex(1));
  const char *signature = lua_tostring (L, lua_upvalueindex(2));
  int argc = 2, f_count = 0, b_count = 0;
  struct cmd_call_stack s[1];
  const char *fc;

  for (fc = signature; fc[0]; fc++)
    {
      switch (fc[0])
	{
	case 'f':
	  s->f[f_count++] = gs_check_number (L, argc++, FP_CHECK_NORMAL);
	  break;
	case 'b':
	  if (lua_isboolean (L, argc))
	    s->b[b_count++] = lua_toboolean (L, argc++);
	  else
	    return luaL_error (L, "expected boolean for argument #%i", argc);
	}
    }

  pthread_mutex_lock (agg_mutex);
  path_cmd (p, id, s);
  pthread_mutex_unlock (agg_mutex);
  return 0;
}

int
agg_path_index (lua_State *L)
{
  struct path_cmd_reg *r;
  const char *key;

  if (! lua_isstring (L, 2))
    return 0;

  key = lua_tostring (L, 2);
  for (r = cmd_table; r->cmd; r++)
    {
      if (strcmp (key, r->cmd) == 0)
	break;
    }

  if (r->cmd)
    {
      lua_pushinteger (L, (int) r->id);
      lua_pushstring (L, r->signature);
      lua_pushcclosure (L, agg_path_cmd, 2);
      return 1;
    }

  return 0;
}

int
agg_ellipse_new (lua_State *L)
{
  draw::ellipse *vs = push_new_object<draw::ellipse>(L, GS_DRAW_ELLIPSE);
  double x = luaL_checknumber (L, 1);
  double y = luaL_checknumber (L, 2);
  double rx = luaL_checknumber (L, 3);
  double ry = luaL_checknumber (L, 4);
  vs->self().init(x, y, rx, ry, 0, false);
  return 1;
}

int
agg_circle_new (lua_State *L)
{
  draw::ellipse *vs = push_new_object<draw::ellipse>(L, GS_DRAW_ELLIPSE);
  double x = luaL_checknumber (L, 1);
  double y = luaL_checknumber (L, 2);
  double r = luaL_checknumber (L, 3);
  vs->self().init(x, y, r, r, 0, false);
  return 1;
}

int
agg_ellipse_free (lua_State *L)
{
  return object_free<draw::ellipse>(L, 1, GS_DRAW_ELLIPSE);
}

int
textshape_new (lua_State *L)
{
  double x = luaL_checknumber(L, 1);
  double y = luaL_checknumber(L, 2);
  const char* text = luaL_checkstring(L, 3);
  double ts = luaL_checknumber(L, 4);
  new(L, GS_DRAW_TEXTSHAPE) draw::text_shape(x, y, text, ts);
  return 1;
}

int
textshape_free (lua_State *L)
{
  return object_free<draw::text_shape>(L, 1, GS_DRAW_TEXTSHAPE);
}

int
marker_new (lua_State *L)
{
  const double x = luaL_checknumber(L, 1);
  const double y = luaL_checknumber(L, 2);
  const char *sym_name = luaL_optstring(L, 3, "");
  const double size = luaL_optnumber(L, 4, 5.0);

  bool stroke;
  sg_object* sym = new_marker_symbol_raw(sym_name, stroke);
  draw::marker* marker = new draw::marker(x, y, sym, size);

  if (stroke)
    new(L, GS_DRAW_MARKER) trans::stroke(marker);
  else
    new(L, GS_DRAW_MARKER) sg_object_ref<manage_owner>(marker);

  return 1;
}

int
marker_free (lua_State *L)
{
  return object_free<sg_object>(L, 1, GS_DRAW_MARKER);
}

void
draw_register (lua_State *L)
{
  luaL_newmetatable (L, GS_METATABLE(GS_DRAW_PATH));
  luaL_register (L, NULL, agg_path_methods);
  lua_pop (L, 1);

  luaL_newmetatable (L, GS_METATABLE(GS_DRAW_ELLIPSE));
  luaL_register (L, NULL, agg_ellipse_methods);
  lua_pop (L, 1);

  luaL_newmetatable (L, GS_METATABLE(GS_DRAW_TEXTSHAPE));
  luaL_register (L, NULL, textshape_methods);
  lua_pop (L, 1);

  luaL_newmetatable (L, GS_METATABLE(GS_DRAW_MARKER));
  luaL_register (L, NULL, marker_methods);
  lua_pop (L, 1);

  /* gsl module registration */
  luaL_register (L, NULL, draw_functions);
}
