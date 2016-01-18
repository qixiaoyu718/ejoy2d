#include "spritepack.h"
#include "sprite.h"
#include "label.h"
#include "shader.h"
#include "particle.h"
#include "material.h"
#include "dfont.h"
#include "render.h"
#include "texture.h"

#include <lua.h>
#include <lauxlib.h>
#include <string.h>

#define SRT_X 1
#define SRT_Y 2
#define SRT_SX 3
#define SRT_SY 4
#define SRT_ROT 5
#define SRT_SCALE 6

static struct render *R = NULL;
static int view_srt_dirty = true;

void
lsprite_initrender(struct render* r) {
	R = r;
}

// index要>0, 不能是相对值
static void
get_reftable(lua_State *L, int index) {
    lua_getuservalue(L, index);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_createtable(L, 0, 1);
        lua_pushvalue(L, -1);
        lua_setuservalue(L, index);
    }
}

static struct sprite *
newlabel(lua_State *L, struct pack_label *label) {
	int sz = sizeof(struct sprite) + sizeof(struct pack_label);
	struct sprite *s = (struct sprite *)lua_newuserdata(L, sz);
	s->parent = NULL;
	// label never has a child
	struct pack_label * pl = (struct pack_label *)(s+1);
	*pl = *label;
	s->pack = NULL;
	s->s.label = pl;
	s->t.mat = NULL;
	s->t.color = 0xffffffff;
	s->t.additive = 0;
	s->t.program = PROGRAM_DEFAULT;
	s->flags = 0;
	s->name = NULL;
	s->id = 0;
	s->type = TYPE_LABEL;
	s->start_frame = 0;
	s->total_frame = 0;
	s->frame = 0;
	s->data.rich_text = NULL;
	s->material = NULL;
	return s;
}

/*
	integer width
	integer height
	integer size
	uinteger color
	string l/r/c

	ret: userdata
 */
static int
lnewlabel(lua_State *L) {
	struct pack_label label;
	label.width = (int)luaL_checkinteger(L,1);
	label.height = (int)luaL_checkinteger(L,2);
	label.size = (int)luaL_checkinteger(L,3);
	label.color = (uint32_t)luaL_optinteger(L,4,0xffffffff);
	const char * align = lua_tostring(L,5);
	label.space_w = (int)lua_tointeger(L, 6);
	label.space_h = (int)lua_tointeger(L, 7);
	label.auto_scale = (int)lua_tointeger(L, 8);
	label.edge = (int)lua_tointeger(L, 9);
	if (align == NULL) {
        label.align = LABEL_ALIGN_DEFAULT;
	} else {
		switch(align[0]) {
		case 'l':
		case 'L':
			label.align = LABEL_ALIGN_H_LEFT_MASK;
			break;
		case 'r':
		case 'R':
			label.align = LABEL_ALIGN_H_RIGHT_MASK;
			break;
		case 'c':
		case 'C':
			label.align = LABEL_ALIGN_H_CENTER_MASK;
			break;
		default:
			return luaL_error(L, "HAlign must be l/r/c");
		}
        switch(align[1]) {
            case 't':
            case 'T':
                label.align |= LABEL_ALIGN_V_TOP_MASK;
                break;
            case 'c':
            case 'C':
                label.align |= LABEL_ALIGN_V_CENTER_MASK;
                break;
            case 'B':
            case 'b':
                label.align |= LABEL_ALIGN_V_BOTTOM_MASK;
                break;
            default:
                return luaL_error(L, "VAlign must be t/c/b");
        }
	}
	newlabel(L, &label);
	return 1;
}

static double
readkey(lua_State *L, int idx, int key, double def) {
	lua_pushvalue(L, lua_upvalueindex(key));
	lua_rawget(L, idx);
	double ret = luaL_optnumber(L, -1, def);
	lua_pop(L,1);
	return ret;
}

static void
fill_srt(lua_State *L, struct srt *srt, int idx) {
	if (lua_isnoneornil(L, idx)) {
		srt->offx = 0;
		srt->offy = 0;
		srt->rot = 0;
		srt->scalex = 1024;
		srt->scaley = 1024;
		return;
	}
	luaL_checktype(L,idx,LUA_TTABLE);
	double x = readkey(L, idx, SRT_X, 0);
	double y = readkey(L, idx, SRT_Y, 0);
	double scale = readkey(L, idx, SRT_SCALE, 0);
	double sx;
	double sy;
	double rot = readkey(L, idx, SRT_ROT, 0);
	if (scale > 0) {
		sx = sy = scale;
	} else {
		sx = readkey(L, idx, SRT_SX, 1);
		sy = readkey(L, idx, SRT_SY, 1);
	}
	srt->offx = x*SCREEN_SCALE;
	srt->offy = y*SCREEN_SCALE;
	srt->scalex = sx*1024;
	srt->scaley = sy*1024;
	srt->rot = rot * (1024.0 / 360.0);
}

static const char * srt_key[] = {
	"x",
	"y",
	"sx",
	"sy",
	"rot",
	"scale",
};


static void
update_message(struct sprite * s, int parentid, int componentid, int frame) {
	struct sprite_pack * pack = s->pack;
	if (pack == NULL)
		return;
	void ** data = (void **)OFFSET_TO_POINTER(pack, pack->data); 
	struct pack_animation * ani = (struct pack_animation *)data[parentid];
	if (frame < 0 || frame >= ani->frame_number) {
		return;
	}
	struct pack_frame *pframe = OFFSET_TO_POINTER(pack, ani->frame);
	pframe = &pframe[frame];
	int i = 0;
	for (; i < pframe->n; i++) {
		struct pack_part * pp = OFFSET_TO_POINTER(pack, pframe->part);
		pp = &pp[i];
		if (pp->component_id == componentid && pp->touchable) {
			s->flags |= SPRFLAG_MESSAGE;
			return;
		}
	}
}

static struct sprite *
newanchor(lua_State *L) {
	int sz = sizeof(struct sprite) + sizeof(struct anchor_data);
	struct sprite * s = (struct sprite *)lua_newuserdata(L, sz);
	s->parent = NULL;
	s->pack = NULL;
	s->t.mat = NULL;
	s->t.color = 0xffffffff;
	s->t.additive = 0;
	s->t.program = PROGRAM_DEFAULT;
	s->flags = SPRFLAG_INVISIBLE;  // anchor is invisible by default
	s->name = NULL;
	s->id = ANCHOR_ID;
	s->type = TYPE_ANCHOR;
	s->data.anchor = (struct anchor_data *)(s+1);
	s->data.anchor->ps = NULL;
	s->data.anchor->pic = NULL;
	s->s.mat = &s->data.anchor->mat;
	s->material = NULL;
	matrix_identity(s->s.mat);

	return s;
}

static struct sprite *
newsprite(lua_State *L, struct sprite_pack *pack, int id, int cache_enable) {
	if (id == ANCHOR_ID) {
		return newanchor(L);
	}
	int sz = sprite_size(pack, id);
	if (sz == 0) {
		return NULL;
	}
	struct sprite * s = (struct sprite *)lua_newuserdata(L, sz);
	sprite_init(s, pack, id, sz);
	int i;
	for (i=0;;i++) {
		int childid = sprite_component(s, i);
		if (childid < 0)
			break;
		if (i==0) {
			lua_newtable(L);
			lua_pushvalue(L,-1);
			lua_setuservalue(L, -3);	// set uservalue for sprite
		}
		struct sprite *c = newsprite(L, pack, childid, cache_enable);
		if (c) {
			c->name = sprite_childname(s, i);
			sprite_mount(s, i, c);
			update_message(c, id, i, s->frame);
			lua_rawseti(L, -2, i+1);
            
            if (cache_enable && c->type == TYPE_PICTURE) {
                lua_rawgeti(L, -1, i+1);
                get_reftable(L, lua_absindex(L, -1));
                
                // gen cache userdata
                int size = sizeof(struct cache_vp);
                struct cache_vp* cache = (struct cache_vp*)lua_newuserdata(L, size);
                c->data.vp_cache = cache;
                lua_setfield(L, -2, "usr_vp_cache");
                
                lua_pop(L, 2);
            }
		}
	}
	if (i>0) {
		lua_pop(L,1);
	}
	return s;
}

/*
	userdata sprite_pack
	integer id

	ret: userdata sprite
 */
static int
lnew(lua_State *L) {
	struct sprite_pack * pack = (struct sprite_pack *)lua_touserdata(L, 1);
	if (pack == NULL) {
		return luaL_error(L, "Need a sprite pack");
	}
	int id = (int)luaL_checkinteger(L, 2);
    int cache = lua_toboolean(L, 3);
	struct sprite * s = newsprite(L, pack, id, cache);
	if (s) {
		return 1;
	}
	return 0;
}

static struct sprite *
self(lua_State *L) {
	struct sprite * s = (struct sprite *)lua_touserdata(L, 1);
	if (s == NULL) {
		luaL_error(L, "Need sprite");
	}
	return s;
}

static int
lgetframe(lua_State *L) {
	struct sprite * s = self(L);
	lua_pushinteger(L, s->frame);
	return 1;
}

static int
lsetframe(lua_State *L) {
	struct sprite * s = self(L);
	int frame = (int)luaL_checknumber(L,2);
	sprite_setframe(s, frame, false);
	return 0;
}

static int
lsetaction(lua_State *L) {
	struct sprite * s = self(L);
	const char * name = lua_tostring(L,2);
	sprite_action(s, name);
	return 0;
}

static int
lgettotalframe(lua_State *L) {
	struct sprite *s = self(L);
	int f = s->total_frame;
	if (f<=0) {
		f = 0;
	}
	lua_pushinteger(L, f);
	return 1;
}

static int
lgetvisible(lua_State *L) {
	struct sprite *s = self(L);
	lua_pushboolean(L, (s->flags & SPRFLAG_INVISIBLE) == 0);
	return 1;
}

static int
lsetvisible(lua_State *L) {
	struct sprite *s = self(L);
	if (lua_toboolean(L, 2)) {
		s->flags &= ~SPRFLAG_INVISIBLE;
	} else {
		s->flags |= SPRFLAG_INVISIBLE;
	}
	return 0;
}

static int
lgetmessage(lua_State *L) {
	struct sprite *s = self(L);
	lua_pushboolean(L, s->flags & SPRFLAG_MESSAGE);
	return 1;
}

static int
lsetmessage(lua_State *L) {
	struct sprite *s = self(L);
	if(lua_toboolean(L, 2)) {
		s->flags |= SPRFLAG_MESSAGE;
	} else {
		s->flags &= ~SPRFLAG_MESSAGE;
	}
	return 0;
}

static int
lget_force_inherit_frame(lua_State *L) {
	struct sprite *s = self(L);
	lua_pushboolean(L, s->flags & SPRFLAG_FORCE_INHERIT_FRAME);
	return 1;
}

static int
lset_force_inherit_frame(lua_State *L) {
	struct sprite *s = self(L);
	if(lua_toboolean(L, 2)) {
		s->flags |= SPRFLAG_FORCE_INHERIT_FRAME;
	} else {
		s->flags &= ~SPRFLAG_FORCE_INHERIT_FRAME;
	}
	return 0;
}

static int
lsetmat(lua_State *L) {
	struct sprite *s = self(L);
	struct matrix *m = (struct matrix *)lua_touserdata(L, 2);
	if (m == NULL)
		return luaL_error(L, "Need a matrix");
	s->t.mat = &s->mat;
	s->mat = *m;

	return 0;
}

static int
lgetmat(lua_State *L) {
	struct sprite *s = self(L);
	if (s->t.mat == NULL) {
		s->t.mat = &s->mat;
		matrix_identity(&s->mat);
	}
	lua_pushlightuserdata(L, s->t.mat);
	return 1;
}

static int
lgetwmat(lua_State *L) {
	struct sprite *s = self(L);
	if (s->type == TYPE_ANCHOR) {
		lua_pushlightuserdata(L, s->s.mat);
		return 1;
	}
	return luaL_error(L, "Only anchor can get world matrix");
}

static int
lgetwpos(lua_State *L) {
	struct sprite *s = self(L);
	if (s->type == TYPE_ANCHOR) {
		struct matrix* mat = s->s.mat;
		lua_pushnumber(L,mat->m[4] /(float)SCREEN_SCALE);
		lua_pushnumber(L,mat->m[5] /(float)SCREEN_SCALE);
		return 2;
	} else {
		struct srt srt;
		fill_srt(L,&srt,2);

		struct matrix tmp;
		sprite_matrix(s, &tmp);

		int pos[2];
		if (sprite_pos(s, &srt, &tmp, pos) == 0) {
			lua_pushinteger(L, pos[0]);
			lua_pushinteger(L, pos[1]);
			return 2;
		} else {
			return 0;
		}
	}
	return luaL_error(L, "Only anchor can get world matrix");
}

static int
lsetprogram(lua_State *L) {
	struct sprite *s = self(L);
	if (lua_isnoneornil(L,2)) {
		s->t.program = PROGRAM_DEFAULT;
	} else {
		s->t.program = (int)luaL_checkinteger(L,2);
	}
	if (s->material) {
		s->material = NULL;
		get_reftable(L, 1);
		lua_pushnil(L);
		lua_setfield(L, -2, "material");
	}
	return 0;
}

static int
lsetscissor(lua_State *L) {
	struct sprite *s = self(L);
	if (s->type != TYPE_PANNEL) {
		return luaL_error(L, "Only pannel can set scissor");
	}
	s->data.scissor = lua_toboolean(L,2);
	return 0;
}

static int
lsetautoscale(lua_State *L) {
    struct sprite *s = self(L);
    if (s->type != TYPE_LABEL) {
        return luaL_error(L, "Only label can set auto_scale");
    }

    bool auto_scale = lua_toboolean(L, 2);
    s->s.label->auto_scale = auto_scale;

    return 0;
}

static int
lsetedge(lua_State *L) {
    struct sprite *s = self(L);
    if (s->type != TYPE_LABEL) {
        return luaL_error(L, "Only label can set edge");
    }
    
    bool edge = lua_toboolean(L, 2);
    s->s.label->edge = edge;
    
    return 0;
}

static int
lgetname(lua_State *L) {
	struct sprite *s = self(L);
	if (s->name == NULL)
		return 0;
	lua_pushstring(L, s->name);
	return 1;
}

static int
lgettype(lua_State *L) {
	struct sprite *s = self(L);
	lua_pushinteger(L, s->type);
	return 1;
}

static int
lgetparentname(lua_State *L) {
	struct sprite *s = self(L);
	if (s->parent == NULL)
		return 0;
	lua_pushstring(L, s->parent->name);
	return 1;
}

static int
lhasparent(lua_State *L) {
	struct sprite *s = self(L);
	lua_pushboolean(L, s->parent != NULL);
	return 1;
}

static int
lsettext(lua_State *L) {
	struct sprite *s = self(L);
	if (s->type != TYPE_LABEL) {
		return luaL_error(L, "Only label can set rich text");
	}
	if (lua_isnoneornil(L, 2)) {
		s->data.rich_text = NULL;

		get_reftable(L, 1);
		lua_pushnil(L);   //sprite, nil, uservalue, nil
		lua_setfield(L, -2, "richtext");
		return 0;
	}

    if (lua_isstring(L, 2)) {
        s->data.rich_text = (struct rich_text*)lua_newuserdata(L, sizeof(struct rich_text));
        s->data.rich_text->text = lua_tostring(L, 2);
        s->data.rich_text->count = 0;
		s->data.rich_text->width = 0;
		s->data.rich_text->height = 0;
		s->data.rich_text->fields = NULL;

		lua_createtable(L, 2, 0);
		lua_pushvalue(L, 2);
		lua_rawseti(L, -2, 1);
		lua_pushvalue(L, 3);
		lua_rawseti(L, -2, 2);
		lua_setuservalue(L, 1);
        
        label_size(s->data.rich_text, s->s.label);
        
        return 0;
    }

    s->data.rich_text = NULL;
    if (!lua_istable(L, 2) || lua_rawlen(L, 2) != 6) {
        return luaL_error(L, "rich text must has a table with two items");
    }

    lua_rawgeti(L, 2, 1);
    const char *txt = luaL_checkstring(L, -1);
    lua_pop(L, 1);

    lua_rawgeti(L, 2, 2);
    int cnt = lua_rawlen(L, -1);
    lua_pop(L, 1);

	struct rich_text *rich = (struct rich_text*)lua_newuserdata(L, sizeof(struct rich_text));

	rich->text = txt;
    rich->count = cnt;
	lua_rawgeti(L, 2, 3);
	rich->width = (int)luaL_checkinteger(L, -1);
	lua_pop(L, 1);
	
	lua_rawgeti(L, 2, 4);
	rich->height = (int)luaL_checkinteger(L, -1);
	lua_pop(L, 1);

	lua_rawgeti(L, 2, 5);
	int sprite_count = (int)luaL_checkinteger(L, -1);
	lua_pop(L, 1);
    
    lua_rawgeti(L, 2, 6);
    rich->label_color_enable = lua_toboolean(L, -1);
    lua_pop(L, 1);

	lua_createtable(L, 3 + sprite_count * 2, 0); //sprite, table, userdata, table

	int size = cnt * sizeof(struct label_field);
	rich->fields = (struct label_field*)lua_newuserdata(L, size); // sprite, table, userdata, table, userdata

	struct label_field *fields = rich->fields;
	int i, sc = 0;
    lua_rawgeti(L, 2, 2);
	for (i=0; i<cnt; i++) {
		lua_rawgeti(L, -1, i+1);
		if (!lua_istable(L,-1)) {
			return luaL_error(L, "rich text unit must be table");
		}

		lua_rawgeti(L, -1, 1);  //start
		((struct label_field*)(fields+i))->start = luaL_checkinteger(L, -1);
		lua_pop(L, 1);

        lua_rawgeti(L, -1, 2);  //end
		((struct label_field*)(fields+i))->end = luaL_checkinteger(L, -1);
        lua_pop(L, 1);

		lua_rawgeti(L, -1, 3);  //type
		uint32_t type = luaL_checkinteger(L, -1);
		((struct label_field*)(fields+i))->type = type;
		lua_pop(L, 1);
		
		lua_rawgeti(L, -1, 4); //val
		if (type == RL_COLOR) {
			((struct label_field*)(fields+i))->color = luaL_checkinteger(L, -1);
		} else if (type == RL_SPRITE) {
			if (sc >= sprite_count) {
				return luaL_error(L, "rich text sprite count error %d %d", sprite_count, sc);
			}

			struct sprite *s = (struct sprite *)lua_touserdata(L, -1);
			lua_rawseti(L, 4, sc * 2 + 1);

			struct label_sprite *ls = (struct label_sprite *)lua_newuserdata(L, sizeof(struct label_sprite));
			ls->s = s;

			lua_pushvalue(L, -1);
			lua_rawseti(L, 4, sc * 2 + 2);

			lua_pop(L, 1);

            lua_rawgeti(L, -1, 5);
            ls->w = (int)luaL_checkinteger(L, -1);
            lua_pop(L, 1);

            lua_rawgeti(L, -1, 6);
            ls->h = (int)luaL_checkinteger(L, -1);
            lua_pop(L, 1);

            lua_rawgeti(L, -1, 7);
            ls->dy = (int)luaL_checkinteger(L, -1);

			((struct label_field*)(fields+i))->ls = ls;

			sc++;
        } else {
			((struct label_field*)(fields+i))->val = luaL_checkinteger(L, -1);
		}
		lua_pop(L, 1);

		//extend here

		lua_pop(L, 1);
	}
   lua_pop(L, 1);

   if (sprite_count != sc) {
	   return luaL_error(L, "rich text sprite count error %d %d", sprite_count, sc);
   }
   rich->sprite_count = sc;

	get_reftable(L, 1); // sprite, table, userdata, table, userdata, table

	int count = sc * 2;
	//userdata of rich_text
	lua_pushvalue(L, 3);
	lua_rawseti(L, 4, count + 1);

	//userdata of fields
	lua_pushvalue(L, 5);
	lua_rawseti(L, 4, count + 2);

	//string
	lua_rawgeti(L, 2, 1);
	lua_rawseti(L, 4, count + 3);

	lua_pushvalue(L, 4);
	lua_setfield(L, -2, "richtext");

	s->data.rich_text = rich;
    
    label_size(s->data.rich_text, s->s.label);
    
	return 0;
}

static int
lgettext(lua_State *L) {
	struct sprite *s = self(L);
	if (s->type != TYPE_LABEL) {
		return luaL_error(L, "Only label can get text");
	}
  if (s->data.rich_text) {
    lua_pushstring(L, s->data.rich_text->text);
    return 1;
  }
	return 0;
}

static int
lgetcolor(lua_State *L) {
	struct sprite *s = self(L);
    if (s->type != TYPE_LABEL)
    {
        lua_pushinteger(L, s->t.color);
    }
    else
    {
        lua_pushinteger(L, label_get_color(s->s.label, &s->t));
    }
	return 1;
}

static int
lsetcolor(lua_State *L) {
	struct sprite *s = self(L);
	uint32_t color = luaL_checkinteger(L,2);
	s->t.color = color;
	return 0;
}

static int
lsetalpha(lua_State *L) {
	struct sprite *s = self(L);
	uint8_t alpha = luaL_checkinteger(L, 2);
	s->t.color = (s->t.color & 0x00ffffff) | (alpha << 24);
	return 0;
}

static int
lgetalpha(lua_State *L) {
	struct sprite *s = self(L);
	lua_pushinteger(L, s->t.color >> 24);
	return 1;
}

static int
lgetadditive(lua_State *L) {
	struct sprite *s = self(L);
	lua_pushinteger(L, s->t.additive);
	return 1;
}

static int
lsetadditive(lua_State *L) {
	struct sprite *s = self(L);
	uint32_t additive = luaL_checkinteger(L,2);
	s->t.additive = additive;
	return 0;
}

static int
lgetparent(lua_State *L) {
	struct sprite *s = self(L);
	if (s->parent == NULL)
		return 0;
	lua_getuservalue(L, 1);
	lua_rawgeti(L, -1, 0);
	return 1;
}

static int
lgetprogram(lua_State *L) {
	struct sprite *s = self(L);
	lua_pushinteger(L, s->t.program);
	return 1;
}

static int
lgetmaterial(lua_State *L) {
	struct sprite *s = self(L);
	if (s->material) {
		get_reftable(L,1);
		lua_getfield(L, -1, "material");
		return 1;
	}
	return 0;
}

/*
static int
lgetreftable(lua_State *L) {
	//struct sprite *s = self(L);
	get_reftable(L, 1);
	return 1;
}*/

//static const char * ud_key = "user_data";
static uint32_t ud_key = 0xFFFF;
static int
lgetusrdata(lua_State *L) {
	get_reftable(L, 1);
	lua_rawgetp(L, -1, &ud_key);
	if (lua_isnoneornil(L, -1)) {
		lua_newtable(L);
		lua_pushvalue(L, -1);  //sprite, uservalue, nil, table, table
		
		lua_rawsetp(L, 2, &ud_key);
	}
	return 1;
}

static void
lgetter(lua_State *L) {
	luaL_Reg l[] = {
		{"frame", lgetframe},
		{"frame_count", lgettotalframe },
		{"visible", lgetvisible },
		{"name", lgetname },
		{"type", lgettype },
		{"text", lgettext},
		{"color", lgetcolor },
		{"alpha", lgetalpha },
		{"additive", lgetadditive },
		{"message", lgetmessage },
		{"force_inherit_frame", lget_force_inherit_frame },
		{"matrix", lgetmat },
		{"world_matrix", lgetwmat },
		{"parent_name", lgetparentname },	// todo: maybe unused , use parent instead
		{"has_parent", lhasparent },	// todo: maybe unused , use parent instead
		{"parent", lgetparent },
		{"program", lgetprogram },
		{"material", lgetmaterial },
//		{"ref_table", lgetreftable },
		{"usr_data", lgetusrdata },
		{NULL, NULL},
	};
	luaL_newlib(L,l);
}

static void
lsetter(lua_State *L) {
	luaL_Reg l[] = {
		{"frame", lsetframe},
		{"action", lsetaction},
		{"visible", lsetvisible},
		{"matrix" , lsetmat},
		{"text", lsettext},
		{"color", lsetcolor},
		{"alpha", lsetalpha},
		{"additive", lsetadditive },
		{"message", lsetmessage },
		{"force_inherit_frame", lset_force_inherit_frame },
		{"program", lsetprogram },
		{"scissor", lsetscissor },
        {"auto_scale", lsetautoscale},
        {"edge", lsetedge},
		{NULL, NULL},
	};
	luaL_newlib(L,l);
}

static void
ref_parent(lua_State *L, int index, int parent) {
	get_reftable(L, index);
	lua_pushvalue(L, parent);
	lua_rawseti(L, -2, 0);	// set self to uservalue[0] (parent)
	lua_pop(L, 1);
}

static void
fetch_parent(lua_State *L, int index) {
	lua_getuservalue(L, 1);
	lua_rawgeti(L, -1, index+1);
	// A child may not exist, but the name is valid. (empty dummy child)
	if (!lua_isnil(L, -1)) {
		ref_parent(L, lua_gettop(L), 1);
	}
}

static int
lfetch(lua_State *L) {
	struct sprite *s = self(L);
	const char * name = luaL_checkstring(L,2);
	int index = sprite_child(s, name, 0);
	if (index < 0)
		return 0;
	if ((s->flags & SPRFLAG_MULTIMOUNT) == 0)	{ // multimount has no parent
		fetch_parent(L, index);
	}

	return 1;
}

static int
lfetch_by_index(lua_State *L) {
	struct sprite *s = self(L);
	if (s->type != TYPE_ANIMATION) {
		return luaL_error(L, "Only animation can fetch by index");
	}
	int index = (int)luaL_checkinteger(L, 2);
	struct pack_animation *ani = s->s.ani;
	if (index < 0 || index >= ani->component_number) {
		return luaL_error(L, "Component index out of range:%d", index);
	}

	fetch_parent(L, index);

	return 1;
}

static void
unlink_parent(lua_State *L, struct sprite * child, int idx) {
	lua_getuservalue(L, idx);	// reftable
	lua_rawgeti(L, -1, 0);	// reftable parent
	struct sprite * parent = (struct sprite *)lua_touserdata(L, -1);
	if (parent == NULL) {
		luaL_error(L, "No parent object");
	}
	int index = sprite_child_ptr(parent, child);
	if (index < 0) {
		luaL_error(L, "Invalid child object");
	}
	lua_getuservalue(L, -1);	// reftable parent parentref
	lua_pushnil(L);
	lua_rawseti(L, -2, index+1);
	lua_pop(L, 3);
	sprite_mount(parent, index, NULL);
}

static int
lmount(lua_State *L) {
	struct sprite *s = self(L);
	const char * name = luaL_checkstring(L,2);
    int start_idx = (int)lua_tointeger(L, 4);
    
	int index = sprite_child(s, name, start_idx);
	if (index < 0) {
		return luaL_error(L, "No child name %s", name);
	}
	lua_getuservalue(L, 1);

	struct sprite * child = (struct sprite *)lua_touserdata(L, 3);

	lua_rawgeti(L, -1, index+1);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
	} else {
		struct sprite * c = (struct sprite *)lua_touserdata(L, -1);
		if (c == child) {
			// mount not change
			return 0;
		}
		if ((c->flags & SPRFLAG_MULTIMOUNT) == 0) {
			// try to remove parent ref
			lua_getuservalue(L, -1);
			if (lua_istable(L, -1)) {
				lua_pushnil(L);
				lua_rawseti(L, -2, 0);
			}
			lua_pop(L, 2);
		} else {
			lua_pop(L, 1);
		}
	}

	if (child == NULL) {
		sprite_mount(s, index, NULL);
		lua_pushnil(L);
		lua_rawseti(L, -2, index+1);
	} else {
		if (child->parent) {
			unlink_parent(L, child, 3);
		}
		sprite_mount(s, index, child);
		lua_pushvalue(L, 3);
		lua_rawseti(L, -2, index+1);

		if ((child->flags & SPRFLAG_MULTIMOUNT) == 0)	{ // multimount has no parent
			// set child's new parent
			ref_parent(L, 3, 1);
		}
	}
	return 0;
}

/*
	userdata sprite
	table { .x .y .sx .sy .rot }
 */
static int
ldraw(lua_State *L) {
	struct sprite *s = self(L);
	struct srt srt;

	fill_srt(L,&srt,2);
	sprite_draw(s, &srt);
	return 0;
}

static int
ldraw_scene(lua_State *L) {
    screen_draw_scene_begin();

    struct sprite *s = self(L);
    if (view_srt_dirty)
        s->cache_dirty = true;
    sprite_draw(s, NULL);

    screen_draw_scene_end();

    return 0;
}

static int
lset_wrap_mode(lua_State *L) {
    struct sprite *s = self(L);
    float uv_mul[8] = {1.0f, 1.0f,   1.0f, 1.0f,   1.0f, 1.0f,   1.0f, 1.0f};
    if (!lua_isnoneornil(L, 3)) {
        int i = 0;
        for (i = 0; i < 4; i++) {
            lua_rawgeti(L, 3, i*2+1);
            lua_rawgeti(L, 3, i*2+2);
            uv_mul[i*2] = lua_tonumber(L, -2);
            uv_mul[i*2+1] = lua_tonumber(L, -1);
            lua_pop(L, 2);
        }
    }
    sprite_set_texture_wrap(s, (int)lua_tointeger(L, 2), uv_mul);
    
    return 0;
}

static int
laabb(lua_State *L) {
	struct sprite *s = self(L);
	struct srt srt;
	fill_srt(L,&srt,2);
	bool world = lua_toboolean(L, 3);
	
	int aabb[4];
	sprite_aabb(s, &srt, world, aabb);
	int i;
	for (i=0;i<4;i++) {
		lua_pushinteger(L, aabb[i]);
	}
	return 4;
}

static int
lchar_size(lua_State *L) {
	struct sprite *s = self(L);
	if (s->type != TYPE_LABEL) {
		return luaL_error(L, "Ony label can get char_size");
	}
	int idx=0;
	luaL_checktype(L,2,LUA_TTABLE);
	lua_pushinteger(L, s->s.label->width);
	lua_rawseti(L, 2, ++idx);
	lua_pushinteger(L, s->s.label->height);
	lua_rawseti(L, 2, ++idx);
	
	const char* str = NULL;
	if (!lua_isnil(L, 3)) {
		str = lua_tostring(L, 3);
	} else {
		if (!s->data.rich_text || !s->data.rich_text->text) {
			lua_pushinteger(L, idx);
			return 1;
		}
		str = s->data.rich_text->text;
	}
	
	if (!str) {
		lua_pushinteger(L, idx);
		return 1;
	}
	
	int i;
	for (i=0; str[i];) {
		int width=0, height=0, unicode=0;
		int len = label_char_size(s->s.label, str+i, &width, &height, &unicode);
		lua_pushinteger(L, width);
		lua_rawseti(L, 2, ++idx);
		lua_pushinteger(L, height);
		lua_rawseti(L, 2, ++idx);
		lua_pushinteger(L, len);
		lua_rawseti(L, 2, ++idx);
		lua_pushinteger(L, unicode);
		lua_rawseti(L, 2, ++idx);
		i += len;
	}
	lua_pushinteger(L, idx);
	return 1;
}

static int
ltext_size(lua_State *L) {
	struct sprite *s = self(L);
	if (s->type != TYPE_LABEL) {
		return luaL_error(L, "Ony label can get label_size");
	}
	int width = 0, height = 0;
	if (s->data.rich_text != NULL) {
        if (s->data.rich_text->width == 0) {
            label_size(s->data.rich_text, s->s.label);
        }
		width = s->data.rich_text->width;
		height = s->data.rich_text->height;
	}
	lua_pushinteger(L, width);
	lua_pushinteger(L, height);
	lua_pushinteger(L, s->s.label->size);
	return 3;
}

static int
ltext_org_size(lua_State *L) {
    struct sprite *s = self(L);
    if (s->type != TYPE_LABEL) {
        return luaL_error(L, "Ony label can get label_size");
    }
    lua_pushinteger(L, s->s.label->width);
    lua_pushinteger(L, s->s.label->height);
    lua_pushinteger(L, s->s.label->size);
    return 3;
}

static int
lchild_visible(lua_State *L) {
	struct sprite *s = self(L);
	const char * name = luaL_checkstring(L,2);
	lua_pushboolean(L, sprite_child_visible(s, name));
	return 1;
}

static int
lset_child_visible(lua_State *L) {
    struct sprite *s = self(L);
    const char * name = luaL_checkstring(L,2);
    sprite_set_child_visible(s, name, lua_toboolean(L, 3));
    return 0;
}

static int
lset_children_visible(lua_State *L) {
    struct sprite *s = self(L);
    if (s->type != TYPE_ANIMATION)
        return 0;
    const char *name = luaL_checkstring(L,2);
    bool visible = lua_toboolean(L, 3);
    int i;
    struct pack_animation *ani = s->s.ani;
    for (i=0; i<ani->component_number;i++) {
        if (ani->component[i].name && strcmp(ani->component[i].name, name) == 0) {
            struct sprite *child = s->data.children[i];
            if (visible) {
                child->flags &= ~SPRFLAG_INVISIBLE;
            } else {
                child->flags |= SPRFLAG_INVISIBLE;
            }
        }
    }
    return 0;
}

static int
lchildren_name(lua_State *L) {
	struct sprite *s = self(L);
	if (s->type != TYPE_ANIMATION)
		return 0;
	int i;
	int cnt=0;
	struct pack_animation * ani = s->s.ani;
    lua_createtable(L, 0, 0);
	for (i=0;i<ani->component_number;i++) {
		if (ani->component[i].name != NULL) {
			lua_pushstring(L, OFFSET_TO_STRING(s->pack, ani->component[i].name));
            lua_rawseti(L, -2, ++cnt);
		}
	}
	return 1;
}

static int
lset_anchor_particle(lua_State *L) {
	struct sprite *s = self(L);
	if (s->type != TYPE_ANCHOR)
		return luaL_error(L, "need a anchor");

	// ref the ps object and pic to anchor object
	get_reftable(L, 1);
	lua_pushvalue(L, 2);
	lua_rawseti(L, -2, 0);
	lua_pop(L, 1);

	s->data.anchor->ps = (struct particle_system*)lua_touserdata(L, 2);
	struct sprite *p = (struct sprite *)lua_touserdata(L, 3);
	if (p==NULL || p->type != TYPE_PICTURE)
		return luaL_error(L, "need a picture sprite");
	s->data.anchor->pic = p->s.pic;

	return 0;
}

static int
lmatrix_multi_draw(lua_State *L) {
	struct sprite *s = self(L);
	int cnt = (int)luaL_checkinteger(L,3);
	if (cnt == 0)
		return 0;
	luaL_checktype(L,4,LUA_TTABLE);
	luaL_checktype(L,5,LUA_TTABLE);
	if (lua_rawlen(L, 4) < cnt) {
		return luaL_error(L, "matrix length must less then particle count");
	}
	if (lua_rawlen(L, 5) < cnt) {
		return luaL_error(L, "color length must less then particle count");
	}

	struct matrix *mat = (struct matrix *)lua_touserdata(L, 2);

	if (s->t.mat == NULL) {
		s->t.mat = &s->mat;
		matrix_identity(&s->mat);
	}
	struct matrix *parent_mat = s->t.mat;
	uint32_t parent_color = s->t.color;

	int i;
	if (mat) {
		struct matrix tmp;
		for (i = 0; i < cnt; i++) {
			lua_rawgeti(L, 4, i+1);
			lua_rawgeti(L, 5, i+1);
			struct matrix *m = (struct matrix *)lua_touserdata(L, -2);
			matrix_mul(&tmp, m, mat);
			s->t.mat = &tmp;
			s->t.color = (uint32_t)lua_tointeger(L, -1);
			lua_pop(L, 2);

			sprite_draw(s, NULL);
		}
	} else {
		for (i = 0; i < cnt; i++) {
			lua_rawgeti(L, 4, i+1);
			lua_rawgeti(L, 5, i+1);
			struct matrix *m = (struct matrix *)lua_touserdata(L, -2);
			s->t.mat = m;
			s->t.color = (uint32_t)lua_tointeger(L, -1);
			lua_pop(L, 2);

			sprite_draw(s, NULL);
		}
	}

	s->t.mat = parent_mat;
	s->t.color = parent_color;

	return 0;
}

static int
lmulti_draw(lua_State *L) {
	struct sprite *s = self(L);
	int cnt = (int)luaL_checkinteger(L,3);
	if (cnt == 0)
		return 0;
    int n = lua_gettop(L);
	luaL_checktype(L,4,LUA_TTABLE);
	luaL_checktype(L,5,LUA_TTABLE);
	if (lua_rawlen(L, 4) < cnt) {
		return luaL_error(L, "matrix length less then particle count");
	}
    if (n == 6) {
        luaL_checktype(L,6,LUA_TTABLE);
        if (lua_rawlen(L, 6) < cnt) {
            return luaL_error(L, "additive length less then particle count");
        }
    }
	struct srt srt;
	fill_srt(L, &srt, 2);

	if (s->t.mat == NULL) {
		s->t.mat = &s->mat;
		matrix_identity(&s->mat);
	}
	struct matrix *parent_mat = s->t.mat;
	uint32_t parent_color = s->t.color;

	int i;
    if (n == 5) {
        for (i = 0; i < cnt; i++) {
            lua_rawgeti(L, 4, i+1);
            lua_rawgeti(L, 5, i+1);
            s->t.mat = (struct matrix *)lua_touserdata(L, -2);
            s->t.color = (uint32_t)lua_tointeger(L, -1);
            lua_pop(L, 2);

            sprite_draw_as_child(s, &srt, parent_mat, parent_color);
        }
    }else {
        for (i = 0; i < cnt; i++) {
            lua_rawgeti(L, 4, i+1);
            lua_rawgeti(L, 5, i+1);
            lua_rawgeti(L, 6, i+1);
            s->t.mat = (struct matrix *)lua_touserdata(L, -3);
            s->t.color = (uint32_t)lua_tointeger(L, -2);
            s->t.additive = (uint32_t)lua_tointeger(L, -1);
            lua_pop(L, 3);

            sprite_draw_as_child(s, &srt, parent_mat, parent_color);
        }
    }

	s->t.mat = parent_mat;
	s->t.color = parent_color;

	return 0;
}

static struct sprite *
lookup(lua_State *L, struct sprite * spr) {
	int i;
	struct sprite * root = (struct sprite *)lua_touserdata(L, -1);
	lua_getuservalue(L,-1);
	for (i=0;sprite_component(root, i)>=0;i++) {
		struct sprite * child = root->data.children[i];
		if (child) {
			lua_rawgeti(L, -1, i+1);	// parent, reftable, child
			if (child == spr) {
				int child_index = lua_gettop(L);
				int parent_index = child_index - 2;
				ref_parent(L, child_index, parent_index);
				lua_replace(L,-2);	// parent
				return child;
			} else {
				lua_pop(L,1);
			}
		}
	}
	lua_pop(L,1);
	return NULL;
}

static int
unwind(lua_State *L, struct sprite *root, struct sprite *spr) {
	int n = 0;
	while (spr) {
		if (spr == root) {
			return n;
		}
		++n;
		lua_checkstack(L,3);
		lua_pushlightuserdata(L, spr);
		spr = spr->parent;
	}
	return -1;
}

static int
ltest(lua_State *L) {
	struct sprite *s = self(L);
	struct srt srt;
	fill_srt(L,&srt,4);
	float x = luaL_checknumber(L, 2);
	float y = luaL_checknumber(L, 3);
	struct sprite * m = sprite_test(s, &srt, x*SCREEN_SCALE, y*SCREEN_SCALE);
	if (m == NULL)
		return 0;
	if (m==s) {
		lua_settop(L,1);
		return 1;
	}
	lua_settop(L,1);
	int depth = unwind(L, s , m);
	if (depth < 0) {
		return luaL_error(L, "Unwind an invalid sprite");
	}
	int i;
	lua_pushvalue(L,1);
	for (i=depth+1;i>1;i--) {
		struct sprite * tmp = (struct sprite *)lua_touserdata(L, i);
		tmp = lookup(L, tmp);
		if (tmp == NULL) {
			return luaL_error(L, "find an invalid sprite");
		}
		lua_replace(L, -2);
	}

	return 1;
}

static int
lps(lua_State *L) {
	struct sprite *s = self(L);
    s->cache_dirty = true;
	struct matrix *m = &s->mat;
	if (s->t.mat == NULL) {
		matrix_identity(m);
		s->t.mat = m;
	}
	int *mat = m->m;
	int n = lua_gettop(L);
	int x,y,scale;
	switch (n) {
	case 4:
		// x,y,scale
		x = luaL_checknumber(L,2) * SCREEN_SCALE;
		y = luaL_checknumber(L,3) * SCREEN_SCALE;
		scale = luaL_checknumber(L,4) * 1024;
		mat[0] = scale;
		mat[1] = 0;
		mat[2] = 0;
		mat[3] = scale;
		mat[4] = x;
		mat[5] = y;
		break;
	case 3:
		// x,y
		x = luaL_checknumber(L,2) * SCREEN_SCALE;
		y = luaL_checknumber(L,3) * SCREEN_SCALE;
		mat[4] = x;
		mat[5] = y;
		break;
	case 2:
		// scale
		scale = luaL_checknumber(L,2) * 1024;
		mat[0] = scale;
		mat[1] = 0;
		mat[2] = 0;
		mat[3] = scale;
		break;
	default:
		return luaL_error(L, "Invalid parm");
	}
	return 0;
}

static int
lsr(lua_State *L) {
	struct sprite *s = self(L);
    s->cache_dirty = true;
	struct matrix *m = &s->mat;
	if (s->t.mat == NULL) {
		matrix_identity(m);
		s->t.mat = m;
	}
	int sx=1024,sy=1024,r=0;
	int n = lua_gettop(L);
	switch (n) {
	case 4:
		// sx,sy,rot
		r = luaL_checknumber(L,4) * (1024.0 / 360.0);
		// go through
	case 3:
		// sx, sy
		sx = luaL_checknumber(L,2) * 1024;
		sy = luaL_checknumber(L,3) * 1024;
		break;
	case 2:
		// rot
		r = luaL_checknumber(L,2) * (1024.0 / 360.0);
		break;
	}
	matrix_sr(m, sx, sy, r);

	return 0;
}

static int
lrecursion_frame(lua_State *L) {
	struct sprite * s = self(L);
	int frame = (int)luaL_checknumber(L,2);
	int f = sprite_setframe(s, frame, true);
	lua_pushinteger(L, f);
	return 1;
}

static int
ldraw_label_only(lua_State *L) {
    sprite_label_only((int)luaL_checkinteger(L,1));
	return 0;
}

static int
lvisible_test(lua_State *L) {
    sprite_screen_visible_test(lua_toboolean(L, 1));
    return 0;
}

static int
lviewport_srt(lua_State *L) {
    struct srt srt;

    double s = luaL_optnumber(L, 1, 1);
    double x = luaL_optnumber(L, 2, 0);
    double y = luaL_optnumber(L, 3, 0);

    srt.offx = x * SCREEN_SCALE;
    srt.offy = y * SCREEN_SCALE;
    srt.scalex = s * 1024;
    srt.scaley = s * 1024;
    srt.rot = 0;

    view_srt_dirty = set_viewport_srt(&srt);
    return 0;
}

static int
lset_dtex_id(lua_State *L) {
    sprite_cur_dtex_id((int)lua_tointeger(L, 1));
    return 0;
}

static int
ldrawscene_st(lua_State *L) {
    float s = luaL_checknumber(L, 1);
    float x = luaL_checknumber(L, 2);
    float y = luaL_checknumber(L, 3);

    sprite_drawscene_st(s, x, y);
    return 0;
}

static int
lcalc_matrix(lua_State *L) {
	struct sprite * s = self(L);
	struct matrix * mat = (struct matrix *)lua_touserdata(L, 2);
	if (mat == NULL) {
		return luaL_argerror(L, 2, "need a matrix");
	}
	struct matrix *local_matrix = s->t.mat;
	if (local_matrix) {
		struct matrix tmp;
		sprite_matrix(s, &tmp);
		matrix_mul(mat, local_matrix, &tmp);
	} else {
		sprite_matrix(s, mat);
	}

	lua_settop(L, 2);
	return 1;
}

static int
lget_pic_tex_coord(lua_State *L) {
	struct sprite * s = self(L);
	if (s->type != TYPE_PICTURE) {
		return luaL_error(L, "Only picture can get tex coord");
	}
	int index = (int)luaL_checkinteger(L, 2);
	struct pack_picture * pic = s->s.pic;
	if (index < 0 || index >= pic->n) {
		return luaL_error(L, "pic rect index out of range:[0,%d)", pic->n);
	}
	struct pack_quad quad = pic->rect[index];
	int i;
	for (i=0; i<4; i++) {
		lua_pushnumber(L, quad.texture_coord[2*i] / 65535.0f);
		lua_pushnumber(L, quad.texture_coord[2*i+1] / 65535.0f);
	}
	return 8;
}

static int
lhas_anim(lua_State *L) {
    struct sprite * s = self(L);
    if (s->type != TYPE_ANIMATION) {
        return luaL_error(L, "Only anim has action info");
    }
    const char* act = luaL_checkstring(L, 2);
    lua_pushboolean(L, sprite_has_action(s, act) != -1);
    return 1;
}

static int
lget_name_index(lua_State *L) {
    struct sprite * s = self(L);
    if (s->type != TYPE_ANIMATION) {
        return 0;
    }
    const char* name = luaL_checkstring(L, 2);
    
    lua_newtable(L);
    int start_idx = 0;
    int count = 1;
    do {
        start_idx = sprite_child(s, name, start_idx);
        if (start_idx >= 0) {
            lua_pushinteger(L, start_idx);
            lua_rawseti(L, -2, count++);
            start_idx = start_idx + 1;
        }
    } while (start_idx >= 0);
    
    return 1;
}


static void
lmethod(lua_State *L) {
	luaL_Reg l[] = {
		{ "fetch", lfetch },
        { "fetch_by_index", lfetch_by_index },
		{ "mount", lmount },
		{ NULL, NULL },
	};
	luaL_newlib(L,l);

	int i;
	int nk = sizeof(srt_key)/sizeof(srt_key[0]);
	for (i=0;i<nk;i++) {
		lua_pushstring(L, srt_key[i]);
	}
	luaL_Reg l2[] = {
		{ "ps", lps },
		{ "sr", lsr },
		{ "draw", ldraw },
        { "draw_scene", ldraw_scene },
		{ "recursion_frame", lrecursion_frame },
		{ "multi_draw", lmulti_draw },
		{ "matrix_multi_draw", lmatrix_multi_draw },
		{ "test", ltest },
		{ "aabb", laabb },
		{ "text_size", ltext_size},
        { "text_src_size", ltext_org_size},
		{ "char_size", lchar_size},
		{ "child_visible", lchild_visible },
        { "set_child_visible", lset_child_visible },
        { "set_children_visible", lset_children_visible },
		{ "children_name", lchildren_name },
		{ "world_pos", lgetwpos },
		{ "anchor_particle", lset_anchor_particle },
		{ "calc_matrix", lcalc_matrix },
		{ "pic_tex_coord", lget_pic_tex_coord },
        { "has_anim", lhas_anim },
        { "set_wrap_mode", lset_wrap_mode },
        { "get_name_index", lget_name_index },
		{ NULL, NULL, },
	};
	luaL_setfuncs(L,l2,nk);
}

struct dummy_pack {
	struct sprite_pack dummy;
	char name[8];
	struct pack_part part;
	struct pack_frame frame;
	struct pack_action action;
	struct pack_animation animation;
};

static int
lnewproxy(lua_State *L) {
  	static struct dummy_pack dp = {
		{ 0 , 0, 0, { 0, 0 } },	// dummy
		"proxy", // name
		{	// part
			{
				0,	// mat
				0xffffffff,	// color
				0,	// additive
			},	// trans
			0,	// component_id
			0,	// touchable
		},
		{	// frame
            offsetof(struct dummy_pack, part),
			1,	// n
		},
		{	// action
			0,	// name
			1,	// number
			0,	// start_frame
		},
		{	// animation
			offsetof(struct dummy_pack, frame),
			offsetof(struct dummy_pack, action),
			1,	// frame_number
			1,	// action_number
			1,	// component_number
			{{
				offsetof(struct dummy_pack, name),
				0,	// id
			}},
		}
	};
	struct sprite * s = (struct sprite *)lua_newuserdata(L, sizeof(struct sprite));
	lua_newtable(L);
	lua_setuservalue(L, -2);
	s->parent = NULL;
	s->pack = &dp.dummy;
	s->s.ani = &dp.animation;
	s->t.mat = NULL;
	s->t.color = 0xffffffff;
	s->t.additive = 0;
	s->t.program = PROGRAM_DEFAULT;
	s->flags = SPRFLAG_MULTIMOUNT;
	s->name = NULL;
	s->id = 0;
	s->type = TYPE_ANIMATION;
	s->start_frame = 0;
	s->total_frame = 0;
	s->frame = 0;
	s->material = NULL;
	s->data.children[0] = NULL;
	sprite_action(s, NULL);

	return 1;
}

static int
lnewmaterial(lua_State *L) {
	struct sprite *s = self(L);
	int sz = sprite_material_size(s);
	if (sz == 0)
		return luaL_error(L, "The program has not material");
	get_reftable(L, 1);	

	lua_createtable(L, 0, 1);
	void * m = lua_newuserdata(L, sz); // sprite, uservalue, table, matertial
	s->material = (struct material*)m;
	material_init(m, sz, s->t.program);
	lua_setfield(L, -2, "__obj");

	lua_pushvalue(L, -1);	// sprite, uservalue, table, table
	lua_setfield(L, -3, "material");
	lua_pushinteger(L, s->t.program);

	return 2;	// return table, program
}

static struct dfont*
get_dfont(lua_State *L) {
	lua_getfield(L, 1, "__obj");
	struct dfont *df = (struct dfont*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	return df;
}

static int
lnewdfont(lua_State *L) {
	int width = luaL_checkinteger(L, 1);
	int height = luaL_checkinteger(L, 2);
	int format = luaL_checkinteger(L, 3);
	int id = luaL_checkinteger(L, 4);
	
	lua_createtable(L, 0, 1);
	size_t size = dfont_data_size(width, height);
	void * d = lua_newuserdata(L, size);
	dfont_init(d, width, height);
	lua_setfield(L, -2, "__obj");
	
	const char* err = texture_load(id, (enum TEXTURE_FORMAT)format, width, height, NULL, 0);
	if (err) {
		return luaL_error(L, err);
	}
	
	RID tex = texture_glid(id);
	render_texture_update(R, tex, width, height, NULL, 0, 0);
	lua_pushinteger(L, id);
	lua_setfield(L, -2, "texture");
	
	return 1;
}

static int
ldeldfont(lua_State *L) {
	struct dfont *df = get_dfont(L);
	if (!df) {
		return luaL_error(L, "invalid dfont table");
	}

	lua_getfield(L, 1, "texture");
	int tid = luaL_checkinteger(L, -1);
	lua_pop(L, 1);
	
	texture_unload(tid);
	
	return 0;
}

static int
ldfont_flush(lua_State *L) {
	struct dfont *df = get_dfont(L);
	if (!df) {
		return luaL_error(L, "invalid dfont table");
	}
	
	dfont_flush(df);
	return 0;
}

static int
ldfont_lookup(lua_State *L) {
	struct dfont *df = get_dfont(L);
	if (!df) {
		return luaL_error(L, "invalid dfont table");
	}
	
	int id = luaL_checkinteger(L, 2);
	int rect_size = luaL_checkinteger(L, 3);
	
	const struct dfont_rect * rect = dfont_lookup(df, id, rect_size, 0);
	
	if (rect) {
		lua_pushinteger(L, rect->x);
		lua_pushinteger(L, rect->y);
		lua_pushinteger(L, rect->w);
		lua_pushinteger(L, rect->h);
		return 4;
	} else {
		return 0;
	}
}

static int
ldfont_remove(lua_State *L) {
	struct dfont *df = get_dfont(L);
	if (!df) {
		return luaL_error(L, "invalid dfont table");
	}
	
	int id = luaL_checkinteger(L, 2);
	int rect_size = luaL_checkinteger(L, 3);
	dfont_remove(df, id, rect_size, 0);
	
	return 0;
}

static int
ldfont_insert(lua_State *L) {
	struct dfont *df = get_dfont(L);
	if (!df) {
		return luaL_error(L, "invalid dfont table");
	}
	
	lua_getfield(L, 1, "texture");
	int tid = luaL_checkinteger(L, -1);
	lua_pop(L, 1);
	RID tex = texture_glid(tid);
	
	int id = luaL_checkinteger(L, 2);
	int rect_size = luaL_checkinteger(L, 3);
	int w = luaL_checkinteger(L, 4);
	int h = luaL_checkinteger(L, 5);
	void* buff = lua_touserdata(L, 6);
	
	const struct dfont_rect * rect = dfont_lookup(df, id, rect_size, 0);
	if (rect==NULL) {
		rect = dfont_insert(df, id, rect_size, w, h, 0);
		if (rect) {
			render_texture_subupdate(R, tex, buff, rect->x, rect->y, rect->w, rect->h);
		}
	}
	
	if (rect) {
		lua_pushinteger(L, rect->x);
		lua_pushinteger(L, rect->y);
		lua_pushinteger(L, rect->w);
		lua_pushinteger(L, rect->h);
		return 4;
	} else {
		return 0;
	}
}

static void
ldfont_mothod(lua_State *L) {
	luaL_Reg l[] = {
		{"insert", ldfont_insert},
		{"lookup", ldfont_lookup},
		{"remove", ldfont_remove},
		{"flush", ldfont_flush},
		{NULL,NULL},
	};
	luaL_newlib(L, l);
}

int
ejoy2d_sprite(lua_State *L) {
	luaL_Reg l[] ={
		{ "new", lnew },
		{ "label", lnewlabel },
		{ "proxy", lnewproxy },
		{ "dfont", lnewdfont },
		{ "delete_dfont", ldeldfont },
		{ "new_material", lnewmaterial },
        { "draw_label_only", ldraw_label_only },
        { "visible_test", lvisible_test },
        { "viewport_srt", lviewport_srt },
        { "set_dtex_id", lset_dtex_id },
        { "drawscene_st", ldrawscene_st },
		{ NULL, NULL },
	};
	luaL_newlib(L,l);

	lmethod(L);
	lua_setfield(L, -2, "method");
	lgetter(L);
	lua_setfield(L, -2, "get");
	lsetter(L);
	lua_setfield(L, -2, "set");
	ldfont_mothod(L);
	lua_setfield(L, -2, "dfont_method");

	return 1;
}
