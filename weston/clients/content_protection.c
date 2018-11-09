/*
 * Copyright © 2018 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <cairo.h>
#include <sys/time.h>

#include <linux/input.h>
#include <wayland-client.h>
#include "content-protection-client-protocol.h"
#include "window.h"
#include <wayland-client-protocol.h>

#define WIDTH 500
#define HEIGHT 400
#define FRAME_H 18
#define FRAME_W 5
#define BUTTON_WIDTH 50
#define BUTTON_HEIGHT 20

enum content_type {
	TYPE0,
	TYPE1,
	UNPROTECTED = -1
};
struct protected_content_player {
	struct content_protection *cp;
	struct display *display;
	struct window *window;
	struct widget *widget;
	struct button_t *b0, *b1, *off;
	int width, height, x, y;
	enum content_protection_type cp_type;
	bool is_protected;
};

struct button_t {
	struct window *window;
	struct widget *widget;
	struct protected_content_player *pc_player;
	char *name;
};
/**
 * An event to tell the client that there is a change in protection status
 *
 * This event is sent whenever there is a change in content
 * protection. The content protection status can be ON or OFF. ON
 * in case of the desired protection type is accepted on all
 * connectors, and Off in case of any of the connector
 * content-protection property is changed from "enabled"
 */
void handle_status_changed(void *data,
			   struct content_protection *content_protection,
			   int32_t status)
{
	struct protected_content_player *pc_player = data;
	enum content_protection_type event_status = status;

	switch (event_status) {
	case CONTENT_PROTECTION_TYPE_0:
		pc_player->cp_type = CONTENT_PROTECTION_TYPE_0;
		pc_player->is_protected = true;
		break;
	case CONTENT_PROTECTION_TYPE_1:
		pc_player->cp_type = CONTENT_PROTECTION_TYPE_1;
		pc_player->is_protected = true;
		break;
	case CONTENT_PROTECTION_TYPE_UNPROTECTED:
	default:
		pc_player->cp_type = CONTENT_PROTECTION_TYPE_UNPROTECTED;
		pc_player->is_protected = false;
	}
	window_schedule_redraw(pc_player->window);
}

static const struct content_protection_listener pc_player_listener = {
	handle_status_changed,
};

static void
draw_content(cairo_surface_t *surface, int x, int y, int width, int height,
	     enum content_protection_type type)
{
	cairo_t *cr;
	cairo_text_extents_t extents;
	const char *content_text;

	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_rectangle(cr, x, y, width, height);
	if (type == CONTENT_PROTECTION_TYPE_0)
		cairo_set_source_rgba(cr, 0, 1.0, 0, 1.0);
	else if (type == CONTENT_PROTECTION_TYPE_1)
		cairo_set_source_rgba(cr, 0, 0, 1.0, 1.0);
	else
		cairo_set_source_rgba(cr, 1.0, 0, 0, 1.0);
	cairo_fill(cr);

	cairo_set_source_rgba(cr, 0, 0, 0, 1.0);
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
			       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 15);
	if (type == CONTENT_PROTECTION_TYPE_0)
		content_text = "Content-Type : Type-0";
	else if (type == CONTENT_PROTECTION_TYPE_1)
		content_text = "Content-Type : Type-1";
	else
		content_text = "Content-Type : Unprotected";
	cairo_text_extents(cr, content_text, &extents);
	cairo_move_to(cr, width/2 - (extents.width/2),
		      height/2 - (extents.height/2));
	cairo_show_text(cr, content_text);
	cairo_fill(cr);
	cairo_destroy(cr);
}

static void
redraw_handler(struct widget *widget, void *data)
{
	struct protected_content_player *pc_player = data;
	cairo_surface_t *surface;
	struct rectangle rect;

	widget_get_allocation(pc_player->widget, &rect);
	surface = window_get_surface(pc_player->window);
	if (surface == NULL ||
			cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "failed to create cairo egl surface\n");
		return;
	}
	draw_content(surface, rect.x, rect.y, rect.width, rect.height,
		     pc_player->cp_type);
	cairo_surface_destroy(surface);
}

static void
touch_down_handler(struct widget *widget, struct input *input, uint32_t serial,
		   uint32_t time, int32_t id, float x, float y, void *data)
{
	return;
}

static void
resize_handler(struct widget *widget, int32_t width, int32_t height, void *data)
{
	struct rectangle allocation;
	struct protected_content_player *pc_player = data;

	widget_get_allocation(pc_player->widget, &allocation);
	widget_set_allocation(pc_player->b0->widget,
			      allocation.x + 20, allocation.y + 30,
			      BUTTON_WIDTH, BUTTON_HEIGHT);
	widget_set_allocation(pc_player->b1->widget,
			      allocation.x + 20 + BUTTON_WIDTH + 5,
			      allocation.y + 30,
			      BUTTON_WIDTH, BUTTON_HEIGHT);
	widget_set_allocation(pc_player->off->widget,
			      allocation.x + 20 + 2 * (BUTTON_WIDTH + 5),
			      allocation.y + 30,
			      BUTTON_WIDTH, BUTTON_HEIGHT);
}

static void
buttons_handler(struct widget *widget, struct input *input, uint32_t time,
		uint32_t button, enum wl_pointer_button_state state, void *data)
{
	struct button_t *b = data;
	struct protected_content_player *pc_player = b->pc_player;
	enum content_protection_type request_type;

	if (strcmp(b->name, "TYPE-0") == 0)
		request_type = CONTENT_PROTECTION_TYPE_0;
	else if (strcmp(b->name, "TYPE-1") == 0)
		request_type = CONTENT_PROTECTION_TYPE_1;
	else
		request_type = CONTENT_PROTECTION_TYPE_UNPROTECTED;
	if (request_type == CONTENT_PROTECTION_TYPE_0 ||
	    request_type == CONTENT_PROTECTION_TYPE_1)
		content_protection_desired(pc_player->cp, request_type);
	else
		content_protection_disable(pc_player->cp);
}

static void
handle_global(struct display *display, uint32_t name, const char *interface,
	      uint32_t version, void *data)
{
	struct protected_content_player *pc_player = data;

	if (strcmp(interface, "content_protection") == 0) {
		pc_player->cp = display_bind(display, name,
					     &content_protection_interface, 1);
		content_protection_add_listener(pc_player->cp,
						&pc_player_listener, pc_player);
	}
}

static void
buttons_redraw_handler(struct widget *widget, void *data)
{
	struct button_t *b = data;
	cairo_surface_t *surface;
	struct rectangle allocation;
	cairo_t *cr;

	surface = window_get_surface(b->window);
	widget_get_allocation(b->widget, &allocation);

	cr = cairo_create(surface);
	cairo_rectangle(cr, allocation.x, allocation.y, allocation.width,
			allocation.height);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_fill(cr);

	cairo_set_source_rgba(cr, 0, 0, 0, 1.0);
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
			CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 12);
	cairo_move_to(cr, allocation.x + 5, allocation.y + 15);
	cairo_show_text(cr, b->name);
	cairo_fill(cr);

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

struct button_t *create_button(struct protected_content_player *pc_player,
			       const char *name)
{
	struct button_t *b;

	b = zalloc(sizeof(struct button_t));
	if (b == NULL) {
		fprintf(stderr, "Failed to allocate memory for button.\n");
		exit(0);
	}
	b->widget = widget_add_widget(pc_player->widget, b);
	b->window = pc_player->window;
	b->pc_player = pc_player;
	b->name = name;
	widget_set_redraw_handler(b->widget, buttons_redraw_handler);
	widget_set_button_handler(b->widget, buttons_handler);
	return b;
}

static void free_pc_player(struct protected_content_player *pc_player)
{
	if (!pc_player)
		return;
	if (pc_player->b0) {
		free(pc_player->b0);
		pc_player->b0 = NULL;
	}
	if (pc_player->b1) {
		free(pc_player->b1);
		pc_player->b1 = NULL;
	}
	if (pc_player->off) {
		free(pc_player->off);
		pc_player->off = NULL;
	}
	free(pc_player);
}

int main(int argc, char *argv[])
{
	struct protected_content_player *pc_player;
	struct display *d;
	static const char str_type_0[] = "TYPE-0";
	static const char str_type_1[] = "TYPE-1";
	static const char str_type_off[] = "OFF";

	pc_player = zalloc(sizeof(struct protected_content_player));
	if (pc_player == NULL) {
		fprintf(stderr, "failed to allocate memory: %m\n");
		return -1;
	}
	d = display_create(&argc, argv);
	if (d == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}
	pc_player->cp_type = CONTENT_PROTECTION_TYPE_UNPROTECTED;
	pc_player->is_protected = false;
	pc_player->width = WIDTH * 2.0/4.0;
	pc_player->height = HEIGHT * 2.0/4.0;
	pc_player->x = WIDTH * 1.0/4.0;
	pc_player->y = HEIGHT * 1.0/4.0;
	pc_player->window = window_create(d);
	pc_player->widget = window_frame_create(pc_player->window, pc_player);
	pc_player->display = d;
	display_set_user_data(d, pc_player);
	display_set_global_handler(d, handle_global);

	pc_player->b0 = create_button(pc_player, str_type_0);
	pc_player->b1 = create_button(pc_player, str_type_1);
	pc_player->off = create_button(pc_player, str_type_off);

	window_set_title(pc_player->window, "Player");
	widget_set_redraw_handler(pc_player->widget, redraw_handler);
	widget_set_resize_handler(pc_player->widget, resize_handler);
	widget_set_touch_down_handler(pc_player->widget, touch_down_handler);
	window_schedule_resize(pc_player->window, WIDTH, HEIGHT);
	widget_schedule_redraw(pc_player->b0->widget);
	widget_schedule_redraw(pc_player->b1->widget);
	widget_schedule_redraw(pc_player->off->widget);

	if (pc_player->cp == NULL) {
		printf("The content-protection object is NULL\n");
		return -1;
	}

	/* Disable the content-protection in the begining */
	content_protection_disable(pc_player->cp);
	display_run(d);

	widget_destroy(pc_player->b0->widget);
	widget_destroy(pc_player->b1->widget);
	widget_destroy(pc_player->off->widget);
	widget_destroy(pc_player->widget);
	window_destroy(pc_player->window);
	display_destroy(d);
	free_pc_player(pc_player);
	return 0;
}
