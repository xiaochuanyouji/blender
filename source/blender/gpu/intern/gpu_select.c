/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2014 Blender Foundation.
 * All rights reserved.
 *
 * Contributor(s): Antony Riakiotakis.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/gpu/intern/gpu_select.c
 *  \ingroup gpu
 *
 * Interface for accessing gpu-related methods for selection. The semantics will be
 * similar to glRenderMode(GL_SELECT) since the goal is to maintain compatibility.
 */
#include "GPU_select.h"
#include "GPU_extensions.h"
#include "GPU_glew.h"
 
#include "MEM_guardedalloc.h"

#include "DNA_userdef_types.h"

#include "BLI_rect.h"

#include "BLI_utildefines.h"

/* Ad hoc number of queries to allocate to skip doing many glGenQueries */
#define ALLOC_QUERIES 200

typedef struct GPUQueryState {
	/* To ignore selection id calls when not initialized */
	bool select_is_active;
	/* Tracks whether a query has been issued so that gpu_load_id can end the previous one */
	bool query_issued;
	/* array holding the OpenGL query identifiers */
	unsigned int *queries;
	/* array holding the id corresponding to each query */
	unsigned int *id;
	/* number of queries in *queries and *id */
	unsigned int num_of_queries;
	/* index to the next query to start */
	unsigned int active_query;
	/* flag to cache user preference for occlusion based selection */
	bool use_gpu_select;
	/* cache on initialization */
	unsigned int *buffer;
	/* buffer size (stores number of integers, for actual size multiply by sizeof integer)*/
	unsigned int bufsize;
	/* mode of operation */
	char mode;
	unsigned int index;
	int oldhits;
} GPUQueryState;

static GPUQueryState g_query_state = {0};

/**
 * initialize and provide buffer for results
 */
void GPU_select_begin(unsigned int *buffer, unsigned int bufsize, const rcti *input, char mode, int oldhits)
{
	g_query_state.select_is_active = true;
	g_query_state.query_issued = false;
	g_query_state.active_query = 0;
	g_query_state.use_gpu_select = GPU_select_query_check_active();
	g_query_state.num_of_queries = 0;
	g_query_state.bufsize = bufsize;
	g_query_state.buffer = buffer;
	g_query_state.mode = mode;
	g_query_state.index = 0;
	g_query_state.oldhits = oldhits;

	if (!g_query_state.use_gpu_select) {
		glSelectBuffer(bufsize, (GLuint *)buffer);
		glRenderMode(GL_SELECT);
		glInitNames();
		glPushName(-1);
	}
	else {
		float viewport[4];

		g_query_state.num_of_queries = ALLOC_QUERIES;

		g_query_state.queries = MEM_mallocN(g_query_state.num_of_queries * sizeof(*g_query_state.queries), "gpu selection queries");
		g_query_state.id = MEM_mallocN(g_query_state.num_of_queries * sizeof(*g_query_state.id), "gpu selection ids");
		glGenQueries(g_query_state.num_of_queries, g_query_state.queries);

		glPushAttrib(GL_DEPTH_BUFFER_BIT | GL_VIEWPORT_BIT);
		/* disable writing to the framebuffer */
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

		/* In order to save some fill rate we minimize the viewport using rect.
		 * We need to get the region of the scissor so that our geometry doesn't
		 * get rejected before the depth test. Should probably cull rect against
		 * scissor for viewport but this is a rare case I think */
		glGetFloatv(GL_SCISSOR_BOX, viewport);
		glViewport(viewport[0], viewport[1], BLI_rcti_size_x(input), BLI_rcti_size_y(input));

		/* occlusion queries operates on fragments that pass tests and since we are interested on all
		 * objects in the view frustum independently of their order, we need to disable the depth test */
		if (mode == GPU_SELECT_ALL) {
			glDisable(GL_DEPTH_TEST);
			glDepthMask(GL_FALSE);
		}
		else if (mode == GPU_SELECT_NEAREST_FIRST_PASS) {
			glClear(GL_DEPTH_BUFFER_BIT);
			glEnable(GL_DEPTH_TEST);
			glDepthMask(GL_TRUE);
			glDepthFunc(GL_LEQUAL);
		}
		else if (mode == GPU_SELECT_NEAREST_SECOND_PASS) {
			glEnable(GL_DEPTH_TEST);
			glDepthMask(GL_FALSE);
			glDepthFunc(GL_EQUAL);
		}
	}
}

/**
 * loads a new selection id and ends previous query, if any. In second pass of selection it also returns
 * if id has been hit on the first pass already.
 * Thus we can skip drawing un-hit objects.
 *
 * \warning We rely on the order of object rendering on passes to be the same for this to work.
 */
bool GPU_select_load_id(unsigned int id)
{
	/* if no selection mode active, ignore */
	if (!g_query_state.select_is_active)
		return true;

	if (!g_query_state.use_gpu_select) {
		glLoadName(id);
	}
	else {
		if (g_query_state.query_issued) {
			glEndQuery(GL_SAMPLES_PASSED);
		}
		/* if required, allocate extra queries */
		if (g_query_state.active_query == g_query_state.num_of_queries) {
			g_query_state.num_of_queries += ALLOC_QUERIES;
			g_query_state.queries = MEM_reallocN(g_query_state.queries, g_query_state.num_of_queries * sizeof(*g_query_state.queries));
			g_query_state.id = MEM_reallocN(g_query_state.id, g_query_state.num_of_queries * sizeof(*g_query_state.id));
			glGenQueries(ALLOC_QUERIES, &g_query_state.queries[g_query_state.active_query]);
		}

		glBeginQuery(GL_SAMPLES_PASSED, g_query_state.queries[g_query_state.active_query]);
		g_query_state.id[g_query_state.active_query] = id;
		g_query_state.active_query++;
		g_query_state.query_issued = true;

		if (g_query_state.mode == GPU_SELECT_NEAREST_SECOND_PASS && g_query_state.index < g_query_state.oldhits) {
			if (g_query_state.buffer[g_query_state.index * 4 + 3] == id) {
				g_query_state.index++;
				return true;
			}
			else {
				return false;
			}
		}
	}

	return true;
}

/**
 * Cleanup and flush selection results to buffer.
 * Return number of hits and hits in buffer.
 * if \a dopass is true, we will do a second pass with occlusion queries to get the closest hit.
 */
unsigned int GPU_select_end(void)
{
	unsigned int hits = 0;
	if (!g_query_state.use_gpu_select) {
		glPopName();
		hits = glRenderMode(GL_RENDER);
	}
	else {
		int i;

		if (g_query_state.query_issued) {
			glEndQuery(GL_SAMPLES_PASSED);
		}

		for (i = 0; i < g_query_state.active_query; i++) {
			unsigned int result;
			glGetQueryObjectuiv(g_query_state.queries[i], GL_QUERY_RESULT, &result);
			if (result > 0) {
				if (g_query_state.mode != GPU_SELECT_NEAREST_SECOND_PASS) {
					int maxhits = g_query_state.bufsize / 4;

					if (hits < maxhits) {
						g_query_state.buffer[hits * 4] = 1;
						g_query_state.buffer[hits * 4 + 1] = 0xFFFF;
						g_query_state.buffer[hits * 4 + 2] = 0xFFFF;
						g_query_state.buffer[hits * 4 + 3] = g_query_state.id[i];

						hits++;
					}
					else {
						hits = -1;
						break;
					}
				}
				else {
					int j;
					/* search in buffer and make selected object first */
					for (j = 0; j < g_query_state.oldhits; j++) {
						if (g_query_state.buffer[j * 4 + 3] == g_query_state.id[i]) {
							g_query_state.buffer[j * 4 + 1] = 0;
							g_query_state.buffer[j * 4 + 2] = 0;
						}
					}
					break;
				}
			}
		}

		glDeleteQueries(g_query_state.num_of_queries, g_query_state.queries);
		MEM_freeN(g_query_state.queries);
		MEM_freeN(g_query_state.id);
		glPopAttrib();
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	}

	g_query_state.select_is_active = false;

	return hits;
}

/**
 * has user activated?
 */
bool GPU_select_query_check_active(void)
{
	return ((U.gpu_select_method == USER_SELECT_USE_OCCLUSION_QUERY) ||
	        ((U.gpu_select_method == USER_SELECT_AUTO) &&
	         (GPU_type_matches(GPU_DEVICE_ATI, GPU_OS_ANY, GPU_DRIVER_ANY) ||
	          /* unsupported by nouveau, gallium 0.4, see: T47940 */
	          GPU_type_matches(GPU_DEVICE_NVIDIA, GPU_OS_UNIX, GPU_DRIVER_OPENSOURCE))));

}
