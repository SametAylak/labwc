/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_WORKSPACES_H
#define LABWC_WORKSPACES_H

#include <stdbool.h>
#include <wayland-util.h>
#include <wayland-server-core.h>

struct seat;
struct server;
struct wlr_scene_tree;

/*
 * A workspace group owns a set of workspaces and tracks which of them is
 * currently active. There is exactly one group, shared by all outputs
 * (see workspaces_init()).
 */
struct workspace_group {
	struct wl_list link; /* struct server.workspaces.groups */

	struct wl_list workspaces; /* struct workspace.link */
	struct workspace *current;
	struct workspace *last;

	struct wlr_ext_workspace_group_handle_v1 *ext_group;
};

struct workspace {
	struct wl_list link; /* struct workspace_group.workspaces */
	struct workspace_group *group;

	char *name;
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *view_trees[3];

	struct wlr_ext_workspace_handle_v1 *ext_workspace;
};

void workspaces_init(void);
void workspaces_switch_to(struct workspace *target, bool update_focus);
void workspaces_destroy(void);
void workspaces_osd_hide(struct seat *seat);
struct workspace *workspaces_find(struct workspace *anchor, const char *name,
	bool wrap);
void workspaces_reconfigure(void);

/*
 * Accessors that resolve via the active group. They exist so that call sites
 * outside workspaces.c do not need to know how the active workspace is tracked.
 */
struct workspace_group *workspaces_get_active_group(void);
struct workspace *workspaces_get_current(void);
struct workspace *workspaces_get_last(void);

#endif /* LABWC_WORKSPACES_H */
