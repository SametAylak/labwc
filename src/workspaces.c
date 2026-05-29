// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "workspaces.h"
#include <assert.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include "buffer.h"
#include "common/font.h"
#include "common/graphic-helpers.h"
#include "common/list.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "input/keyboard.h"
#include "labwc.h"
#include "output.h"
#include "show-desktop.h"
#include "theme.h"
#include "view.h"

#define EXT_WORKSPACES_VERSION 1

/* Internal helpers */
static size_t
parse_workspace_index(const char *name)
{
	/*
	 * We only want to get positive numbers which span the whole string.
	 *
	 * More detailed requirement:
	 *  .---------------.--------------.
	 *  |     Input     | Return value |
	 *  |---------------+--------------|
	 *  | "2nd desktop" |      0       |
	 *  |    "-50"      |      0       |
	 *  |     "0"       |      0       |
	 *  |    "124"      |     124      |
	 *  |    "1.24"     |      0       |
	 *  `------------------------------´
	 *
	 * As atoi() happily parses any numbers until it hits a non-number we
	 * can't really use it for this case. Instead, we use strtol() combined
	 * with further checks for the endptr (remaining non-number characters)
	 * and returned negative numbers.
	 */
	long index;
	char *endptr;
	errno = 0;
	index = strtol(name, &endptr, 10);
	if (errno || *endptr != '\0' || index < 0) {
		return 0;
	}
	return index;
}

static void
_osd_update(void)
{
	struct theme *theme = rc.theme;
	struct workspace_group *group = server.workspaces.active_group;

	/* Settings */
	uint16_t margin = 10;
	uint16_t padding = 2;
	uint16_t rect_height = theme->osd_workspace_switcher_boxes_height;
	uint16_t rect_width = theme->osd_workspace_switcher_boxes_width;
	bool hide_boxes = theme->osd_workspace_switcher_boxes_width == 0 ||
		theme->osd_workspace_switcher_boxes_height == 0;

	/* Dimensions */
	size_t workspace_count = wl_list_length(&group->workspaces);
	uint16_t marker_width = workspace_count * (rect_width + padding) - padding;
	uint16_t width = margin * 2 + (marker_width < 200 ? 200 : marker_width);
	uint16_t height = margin * (hide_boxes ? 2 : 3) + rect_height + font_height(&rc.font_osd);

	cairo_t *cairo;
	cairo_surface_t *surface;
	struct workspace *workspace;

	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (!output_is_usable(output)) {
			continue;
		}
		struct lab_data_buffer *buffer = buffer_create_cairo(width, height,
			output->wlr_output->scale);
		if (!buffer) {
			wlr_log(WLR_ERROR, "Failed to allocate buffer for workspace OSD");
			continue;
		}

		cairo = cairo_create(buffer->surface);

		/* Background */
		set_cairo_color(cairo, theme->osd_bg_color);
		cairo_rectangle(cairo, 0, 0, width, height);
		cairo_fill(cairo);

		/* Border */
		set_cairo_color(cairo, theme->osd_border_color);
		struct wlr_fbox border_fbox = {
			.width = width,
			.height = height,
		};
		draw_cairo_border(cairo, border_fbox, theme->osd_border_width);

		/* Boxes */
		uint16_t x;
		if (!hide_boxes) {
			x = (width - marker_width) / 2;
			wl_list_for_each(workspace, &group->workspaces, link) {
				bool active =  workspace == group->current;
				set_cairo_color(cairo, rc.theme->osd_label_text_color);
				struct wlr_fbox fbox = {
					.x = x,
					.y = margin,
					.width = rect_width,
					.height = rect_height,
				};
				draw_cairo_border(cairo, fbox,
					theme->osd_workspace_switcher_boxes_border_width);
				if (active) {
					cairo_rectangle(cairo, x, margin,
						rect_width, rect_height);
					cairo_fill(cairo);
				}
				x += rect_width + padding;
			}
		}

		/* Text */
		set_cairo_color(cairo, rc.theme->osd_label_text_color);
		PangoLayout *layout = pango_cairo_create_layout(cairo);
		pango_context_set_round_glyph_positions(pango_layout_get_context(layout), false);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

		/* Center workspace indicator on the x axis */
		int req_width = font_width(&rc.font_osd, group->current->name);
		req_width = MIN(req_width, width - 2 * margin);
		x = (width - req_width) / 2;
		if (!hide_boxes) {
			cairo_move_to(cairo, x, margin * 2 + rect_height);
		} else {
			cairo_move_to(cairo, x, (height - font_height(&rc.font_osd)) / 2.0);
		}
		PangoFontDescription *desc = font_to_pango_desc(&rc.font_osd);
		//pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
		pango_layout_set_font_description(layout, desc);
		pango_layout_set_width(layout, req_width * PANGO_SCALE);
		pango_font_description_free(desc);
		pango_layout_set_text(layout, group->current->name, -1);
		pango_cairo_show_layout(cairo, layout);

		g_object_unref(layout);
		surface = cairo_get_target(cairo);
		cairo_surface_flush(surface);
		cairo_destroy(cairo);

		if (!output->workspace_osd) {
			output->workspace_osd = lab_wlr_scene_buffer_create(
				&server.scene->tree, NULL);
		}
		/* Position the whole thing */
		struct wlr_box output_box;
		wlr_output_layout_get_box(server.output_layout,
			output->wlr_output, &output_box);
		int lx = output_box.x + (output_box.width - width) / 2;
		int ly = output_box.y + (output_box.height - height) / 2;
		wlr_scene_node_set_position(&output->workspace_osd->node, lx, ly);
		wlr_scene_buffer_set_buffer(output->workspace_osd, &buffer->base);
		wlr_scene_buffer_set_dest_size(output->workspace_osd,
			buffer->logical_width, buffer->logical_height);

		/* And finally drop the buffer so it will get destroyed on OSD hide */
		wlr_buffer_drop(&buffer->base);
	}
}

static struct workspace *
workspace_find_by_name(struct workspace_group *group, const char *name)
{
	struct workspace *workspace;

	/* by index */
	size_t parsed_index = parse_workspace_index(name);
	if (parsed_index) {
		size_t index = 0;
		wl_list_for_each(workspace, &group->workspaces, link) {
			if (parsed_index == ++index) {
				return workspace;
			}
		}
	}

	/* by name */
	wl_list_for_each(workspace, &group->workspaces, link) {
		if (!strcmp(workspace->name, name)) {
			return workspace;
		}
	}

	wlr_log(WLR_ERROR, "Workspace '%s' not found", name);
	return NULL;
}

static void
handle_ext_workspace_commit(struct wl_listener *listener, void *data)
{
	struct wlr_ext_workspace_v1_commit_event *event = data;

	struct wlr_ext_workspace_v1_request *req;
	wl_list_for_each(req, event->requests, link) {
		if (req->type == WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE) {
			struct workspace *workspace = req->activate.workspace->data;
			workspaces_switch_to(workspace, /* update_focus */ true);
			wlr_log(WLR_INFO, "activating workspace %s", workspace->name);
		}
	}
}

/* Internal API */
static struct workspace_group *
workspace_group_create(void)
{
	struct workspace_group *group = znew(*group);
	wl_list_init(&group->workspaces);
	group->ext_group = wlr_ext_workspace_group_handle_v1_create(
		server.workspaces.ext_manager, /*caps*/ 0);
	wl_list_append(&server.workspaces.groups, &group->link);
	return group;
}

static void
add_workspace(struct workspace_group *group, const char *name)
{
	struct workspace *workspace = znew(*workspace);
	workspace->group = group;
	workspace->name = xstrdup(name);
	workspace->tree = lab_wlr_scene_tree_create(server.workspace_tree);
	workspace->view_trees[VIEW_LAYER_ALWAYS_ON_BOTTOM] =
		lab_wlr_scene_tree_create(workspace->tree);
	workspace->view_trees[VIEW_LAYER_NORMAL] =
		lab_wlr_scene_tree_create(workspace->tree);
	workspace->view_trees[VIEW_LAYER_ALWAYS_ON_TOP] =
		lab_wlr_scene_tree_create(workspace->tree);
	wl_list_append(&group->workspaces, &workspace->link);
	wlr_scene_node_set_enabled(&workspace->tree->node, false);

	workspace->ext_workspace = wlr_ext_workspace_handle_v1_create(
		server.workspaces.ext_manager, /*id*/ NULL,
		EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
	workspace->ext_workspace->data = workspace;
	wlr_ext_workspace_handle_v1_set_group(
		workspace->ext_workspace, group->ext_group);
	wlr_ext_workspace_handle_v1_set_name(workspace->ext_workspace, name);
}

static struct workspace *
get_prev(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	struct wl_list *target_link = current->link.prev;
	if (target_link == workspaces) {
		/* Current workspace is the first one */
		if (!wrap) {
			return NULL;
		}
		/* Roll over */
		target_link = target_link->prev;
	}
	return wl_container_of(target_link, current, link);
}

static struct workspace *
get_next(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	struct wl_list *target_link = current->link.next;
	if (target_link == workspaces) {
		/* Current workspace is the last one */
		if (!wrap) {
			return NULL;
		}
		/* Roll over */
		target_link = target_link->next;
	}
	return wl_container_of(target_link, current, link);
}

static bool
workspace_has_views(struct workspace *workspace)
{
	struct view *view;

	for_each_view(view, &server.views, LAB_VIEW_CRITERIA_NO_OMNIPRESENT) {
		if (view->workspace == workspace) {
			return true;
		}
	}
	return false;
}

static struct workspace *
get_adjacent_occupied(struct workspace *current, struct wl_list *workspaces,
		bool wrap, bool reverse)
{
	struct wl_list *start = &current->link;
	struct wl_list *link = reverse ? start->prev : start->next;
	bool has_wrapped = false;

	while (true) {
		/* Handle list boundaries */
		if (link == workspaces) {
			if (!wrap) {
				break;  /* No wrapping allowed - stop searching */
			}
			if (has_wrapped) {
				break;  /* Already wrapped once - stop to prevent infinite loop */
			}
			/* Wrap around */
			link = reverse ? workspaces->prev : workspaces->next;
			has_wrapped = true;
			continue;
		}

		/* Get the workspace */
		struct workspace *target = wl_container_of(link, target, link);

		/* Check if we've come full circle */
		if (link == start) {
			break;
		}

		/* Check if it's occupied (and not current) */
		if (target != current && workspace_has_views(target)) {
			return target;
		}

		/* Move to next/prev */
		link = reverse ? link->prev : link->next;
	}

	return NULL;  /* No occupied workspace found */
}

static struct workspace *
get_prev_occupied(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	return get_adjacent_occupied(current, workspaces, wrap, true);
}

static struct workspace *
get_next_occupied(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	return get_adjacent_occupied(current, workspaces, wrap, false);
}

static int
_osd_handle_timeout(void *data)
{
	struct seat *seat = data;
	workspaces_osd_hide(seat);
	/* Don't re-check */
	return 0;
}

static void
_osd_show(void)
{
	if (!rc.workspace_config.popuptime) {
		return;
	}

	_osd_update();
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output_is_usable(output) && output->workspace_osd) {
			wlr_scene_node_set_enabled(&output->workspace_osd->node, true);
		}
	}
	if (keyboard_get_all_modifiers(&server.seat)) {
		/* Hidden by release of all modifiers */
		server.seat.workspace_osd_shown_by_modifier = true;
	} else {
		/* Hidden by timer */
		if (!server.seat.workspace_osd_timer) {
			server.seat.workspace_osd_timer = wl_event_loop_add_timer(
				server.wl_event_loop, _osd_handle_timeout, &server.seat);
		}
		wl_event_source_timer_update(server.seat.workspace_osd_timer,
			rc.workspace_config.popuptime);
	}
}

/* Public API */
void
workspaces_init(void)
{
	server.workspaces.ext_manager = wlr_ext_workspace_manager_v1_create(
		server.wl_display, EXT_WORKSPACES_VERSION);

	server.workspaces.on_ext_manager.commit.notify = handle_ext_workspace_commit;
	wl_signal_add(&server.workspaces.ext_manager->events.commit,
		&server.workspaces.on_ext_manager.commit);

	wl_list_init(&server.workspaces.groups);

	/* All outputs share a single workspace group. */
	struct workspace_group *group = workspace_group_create();
	server.workspaces.active_group = group;

	struct workspace_config *conf;
	wl_list_for_each(conf, &rc.workspace_config.workspaces, link) {
		add_workspace(group, conf->name);
	}

	/*
	 * After adding workspaces, check if there is an initial workspace
	 * selected and set that as the initial workspace.
	 */
	char *initial_name = rc.workspace_config.initial_workspace_name;
	struct workspace *initial = NULL;
	struct workspace *first = wl_container_of(
		group->workspaces.next, first, link);

	if (initial_name) {
		initial = workspace_find_by_name(group, initial_name);
	}
	if (!initial) {
		initial = first;
	}

	group->current = initial;
	wlr_scene_node_set_enabled(&initial->tree->node, true);
	wlr_ext_workspace_handle_v1_set_active(initial->ext_workspace, true);
}

/*
 * update_focus should normally be set to true. It is set to false only
 * when this function is called from desktop_focus_view(), in order to
 * avoid unnecessary extra focus changes and possible recursion.
 */
void
workspaces_switch_to(struct workspace *target, bool update_focus)
{
	assert(target);
	struct workspace_group *group = target->group;
	if (target == group->current) {
		return;
	}

	/* Disable the old workspace */
	wlr_scene_node_set_enabled(
		&group->current->tree->node, false);

	wlr_ext_workspace_handle_v1_set_active(
		group->current->ext_workspace, false);

	/*
	 * Move Omnipresent views to new workspace.
	 * Not using for_each_view() since it skips views that
	 * view_is_focusable() returns false (e.g. Conky).
	 */
	struct view *view;
	wl_list_for_each_reverse(view, &server.views, link) {
		if (view->visible_on_all_workspaces) {
			view_move_to_workspace(view, target);
		}
	}

	/* Enable the new workspace */
	wlr_scene_node_set_enabled(&target->tree->node, true);

	/* Save the last visited workspace */
	group->last = group->current;

	/* Make sure new views will spawn on the new workspace */
	group->current = target;

	struct view *grabbed_view = server.grabbed_view;
	if (grabbed_view) {
		view_move_to_workspace(grabbed_view, target);
	}

	/*
	 * Make sure we are focusing what the user sees. Only refocus if
	 * the focus is not already on an omnipresent view.
	 */
	if (update_focus) {
		struct view *active_view = server.active_view;
		if (!(active_view && active_view->visible_on_all_workspaces)) {
			desktop_focus_topmost_view();
		}
	}

	/* And finally show the OSD */
	_osd_show();

	/*
	 * Make sure we are not carrying around a
	 * cursor image from the previous desktop
	 */
	cursor_update_focus();

	/* Ensure that only currently visible fullscreen windows hide the top layer */
	desktop_update_top_layer_visibility();

	wlr_ext_workspace_handle_v1_set_active(target->ext_workspace, true);

	show_desktop_reset();
}

void
workspaces_osd_hide(struct seat *seat)
{
	assert(seat);
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (!output->workspace_osd) {
			continue;
		}
		wlr_scene_node_set_enabled(&output->workspace_osd->node, false);
		wlr_scene_buffer_set_buffer(output->workspace_osd, NULL);
	}
	seat->workspace_osd_shown_by_modifier = false;

	/* Update the cursor focus in case it was on top of the OSD before */
	cursor_update_focus();
}

struct workspace *
workspaces_find(struct workspace *anchor, const char *name, bool wrap)
{
	assert(anchor);
	if (!name) {
		return NULL;
	}
	struct workspace_group *group = anchor->group;
	struct wl_list *workspaces = &group->workspaces;

	if (!strcasecmp(name, "current")) {
		return anchor;
	} else if (!strcasecmp(name, "last")) {
		return group->last;
	} else if (!strcasecmp(name, "left")) {
		return get_prev(anchor, workspaces, wrap);
	} else if (!strcasecmp(name, "right")) {
		return get_next(anchor, workspaces, wrap);
	} else if (!strcasecmp(name, "left-occupied")) {
		return get_prev_occupied(anchor, workspaces, wrap);
	} else if (!strcasecmp(name, "right-occupied")) {
		return get_next_occupied(anchor, workspaces, wrap);
	}
	return workspace_find_by_name(group, name);
}

static void
destroy_workspace(struct workspace *workspace)
{
	wlr_scene_node_destroy(&workspace->tree->node);
	zfree(workspace->name);
	wl_list_remove(&workspace->link);

	wlr_ext_workspace_handle_v1_destroy(workspace->ext_workspace);
	free(workspace);
}

void
workspaces_reconfigure(void)
{
	/*
	 * Compare actual workspace list with the new desired configuration to:
	 *   - Update names
	 *   - Add workspaces if more workspaces are desired
	 *   - Destroy workspaces if fewer workspace are desired
	 */

	struct workspace_group *group = server.workspaces.active_group;
	struct wl_list *workspace_link = group->workspaces.next;

	struct workspace_config *conf;
	wl_list_for_each(conf, &rc.workspace_config.workspaces, link) {
		struct workspace *workspace = wl_container_of(
			workspace_link, workspace, link);

		if (workspace_link == &group->workspaces) {
			/* # of configured workspaces increased */
			wlr_log(WLR_DEBUG, "Adding workspace \"%s\"",
				conf->name);
			add_workspace(group, conf->name);
			continue;
		}
		if (strcmp(workspace->name, conf->name)) {
			/* Workspace is renamed */
			wlr_log(WLR_DEBUG, "Renaming workspace \"%s\" to \"%s\"",
				workspace->name, conf->name);
			xstrdup_replace(workspace->name, conf->name);
			wlr_ext_workspace_handle_v1_set_name(
				workspace->ext_workspace, workspace->name);
		}
		workspace_link = workspace_link->next;
	}

	if (workspace_link == &group->workspaces) {
		return;
	}

	/* # of configured workspaces decreased */
	overlay_finish(&server.seat);
	struct workspace *first_workspace =
		wl_container_of(group->workspaces.next, first_workspace, link);

	while (workspace_link != &group->workspaces) {
		struct workspace *workspace = wl_container_of(
			workspace_link, workspace, link);

		wlr_log(WLR_DEBUG, "Destroying workspace \"%s\"",
			workspace->name);

		struct view *view;
		wl_list_for_each(view, &server.views, link) {
			if (view->workspace == workspace) {
				view_move_to_workspace(view, first_workspace);
			}
		}

		if (group->current == workspace) {
			workspaces_switch_to(first_workspace,
				/* update_focus */ true);
		}
		if (group->last == workspace) {
			group->last = first_workspace;
		}

		workspace_link = workspace_link->next;
		destroy_workspace(workspace);
	}
}

void
workspaces_destroy(void)
{
	struct workspace_group *group, *group_tmp;
	wl_list_for_each_safe(group, group_tmp, &server.workspaces.groups, link) {
		struct workspace *workspace, *tmp;
		wl_list_for_each_safe(workspace, tmp, &group->workspaces, link) {
			destroy_workspace(workspace);
		}
		assert(wl_list_empty(&group->workspaces));
		wl_list_remove(&group->link);
		/*
		 * The ext_group handle is owned by the workspace manager and
		 * torn down together with it when the display is destroyed.
		 */
		free(group);
	}
	assert(wl_list_empty(&server.workspaces.groups));
	wl_list_remove(&server.workspaces.on_ext_manager.commit.link);
}

/* Accessors resolving via the active group */
struct workspace_group *
workspaces_get_active_group(void)
{
	return server.workspaces.active_group;
}

struct workspace *
workspaces_get_current(void)
{
	return workspaces_get_active_group()->current;
}

struct workspace *
workspaces_get_last(void)
{
	return workspaces_get_active_group()->last;
}
