// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <strings.h>
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "labwc.h"
#include "menu/menu.h"
#include "regions.h"
#include "ssd.h"
#include "view.h"
#include "window-rules.h"
#include "workspaces.h"
#include "xwayland.h"

#define LAB_MIN_VIEW_WIDTH  100
#define LAB_MIN_VIEW_HEIGHT  60
#define LAB_FALLBACK_WIDTH  640
#define LAB_FALLBACK_HEIGHT 480

/**
 * All view_apply_xxx_geometry() functions must *not* modify
 * any state besides repositioning or resizing the view.
 *
 * They may be called repeatably during output layout changes.
 */

enum view_edge {
	VIEW_EDGE_INVALID = 0,

	VIEW_EDGE_LEFT,
	VIEW_EDGE_RIGHT,
	VIEW_EDGE_UP,
	VIEW_EDGE_DOWN,
	VIEW_EDGE_CENTER,
};

static enum view_edge
view_edge_invert(enum view_edge edge)
{
	switch (edge) {
	case VIEW_EDGE_LEFT:
		return VIEW_EDGE_RIGHT;
	case VIEW_EDGE_RIGHT:
		return VIEW_EDGE_LEFT;
	case VIEW_EDGE_UP:
		return VIEW_EDGE_DOWN;
	case VIEW_EDGE_DOWN:
		return VIEW_EDGE_UP;
	case VIEW_EDGE_CENTER:
	case VIEW_EDGE_INVALID:
	default:
		return VIEW_EDGE_INVALID;
	}
}

struct edge_manip_hints {
	int left_edge;
	int top_edge;
	int right_edge;
	int bottom_edge;

	int left_max_dx;
	int up_max_dy;
	int right_max_dx;
	int down_max_dy;

	int width_max_dx;
	int height_max_dy;
};

static struct edge_manip_hints
view_get_edge_manip_hints(struct view *view) {
	struct output *output = view->output;
	struct border margin = ssd_get_margin(view->ssd);
	struct wlr_box usable = output_usable_area_in_layout_coords(output);
	if (usable.height == output->wlr_output->height
			&& output->wlr_output->scale != 1) {
		usable.height /= output->wlr_output->scale;
	}
	if (usable.width == output->wlr_output->width
			&& output->wlr_output->scale != 1) {
		usable.width /= output->wlr_output->scale;
	}

	struct edge_manip_hints hints = {
		.left_edge    = view->current.x - margin.left,
		.top_edge     = view->current.y - margin.top,
		.right_edge   = view->current.x + view->pending.width  + margin.right,
		.bottom_edge  = view->current.y + view->pending.height + margin.bottom,

		.left_max_dx  = usable.x + margin.left + rc.gap - view->current.x,
		.up_max_dy    = usable.y + margin.top  + rc.gap - view->current.y,
		.right_max_dx = usable.x + usable.width  - view->pending.width
		                - margin.right  - rc.gap - view->current.x,
		.down_max_dy  = usable.y + usable.height - view->pending.height
		                - margin.bottom - rc.gap - view->current.y,

		.width_max_dx  = MAX(view->current.width  - LAB_MIN_VIEW_WIDTH,  0),
		.height_max_dy = MAX(view->current.height - LAB_MIN_VIEW_HEIGHT, 0),
	};

	return hints;
}

static struct wlr_box
view_get_edge_snap_box(struct view *view, struct output *output,
		enum view_edge edge)
{
	struct wlr_box usable = output_usable_area_in_layout_coords(output);
	if (usable.height == output->wlr_output->height
			&& output->wlr_output->scale != 1) {
		usable.height /= output->wlr_output->scale;
	}
	if (usable.width == output->wlr_output->width
			&& output->wlr_output->scale != 1) {
		usable.width /= output->wlr_output->scale;
	}

	int x_offset = edge == VIEW_EDGE_RIGHT
		? (usable.width + rc.gap) / 2 : rc.gap;
	int y_offset = edge == VIEW_EDGE_DOWN
		? (usable.height + rc.gap) / 2 : rc.gap;

	int base_width, base_height;
	switch (edge) {
	case VIEW_EDGE_LEFT:
	case VIEW_EDGE_RIGHT:
		base_width = (usable.width - 3 * rc.gap) / 2;
		base_height = usable.height - 2 * rc.gap;
		break;
	case VIEW_EDGE_UP:
	case VIEW_EDGE_DOWN:
		base_width = usable.width - 2 * rc.gap;
		base_height = (usable.height - 3 * rc.gap) / 2;
		break;
	default:
	case VIEW_EDGE_CENTER:
		base_width = usable.width - 2 * rc.gap;
		base_height = usable.height - 2 * rc.gap;
		break;
	}

	struct border margin = ssd_get_margin(view->ssd);
	struct wlr_box dst = {
		.x = x_offset + usable.x + margin.left,
		.y = y_offset + usable.y + margin.top,
		.width = base_width - margin.left - margin.right,
		.height = base_height - margin.top - margin.bottom,
	};

	return dst;
}

static void
view_discover_output(struct view *view)
{
	assert(view);
	assert(!view->fullscreen);
	view->output = output_nearest_to(view->server,
		view->current.x + view->current.width / 2,
		view->current.y + view->current.height / 2);
}

static void
_view_set_activated(struct view *view, bool activated)
{
	ssd_set_active(view->ssd, activated);
	if (view->impl->set_activated) {
		view->impl->set_activated(view, activated);
	}
	if (view->toplevel.handle) {
		wlr_foreign_toplevel_handle_v1_set_activated(
			view->toplevel.handle, activated);
	}
}

void
view_set_activated(struct view *view)
{
	assert(view);
	struct view *last = view->server->focused_view;
	if (last == view) {
		return;
	}
	if (last) {
		_view_set_activated(last, false);
	}
	_view_set_activated(view, true);
	view->server->focused_view = view;
}

void
view_set_output(struct view *view, struct output *output)
{
	assert(view);
	assert(!view->fullscreen);
	if (!output_is_usable(output)) {
		wlr_log(WLR_ERROR, "invalid output set for view");
		return;
	}
	view->output = output;
}

void
view_close(struct view *view)
{
	assert(view);
	if (view->impl->close) {
		view->impl->close(view);
	}
}

void
view_move(struct view *view, int x, int y)
{
	assert(view);
	view_move_resize(view, (struct wlr_box){
		.x = x, .y = y,
		.width = view->pending.width,
		.height = view->pending.height
	});
}

void
view_moved(struct view *view)
{
	assert(view);
	wlr_scene_node_set_position(&view->scene_tree->node,
		view->current.x, view->current.y);
	/*
	 * Only floating views change output when moved. Non-floating
	 * views (maximized/tiled/fullscreen) are tied to a particular
	 * output when they enter that state.
	 */
	if (view_is_floating(view)) {
		view_discover_output(view);
	}
	ssd_update_geometry(view->ssd);
	cursor_update_focus(view->server);
	if (view->toplevel.handle) {
		foreign_toplevel_update_outputs(view);
	}
}

void
view_move_resize(struct view *view, struct wlr_box geo)
{
	assert(view);
	if (view->impl->configure) {
		view->impl->configure(view, geo);
	}
}

void
view_resize_relative(struct view *view, int left, int right, int top, int bottom)
{
	if (view->fullscreen || view->maximized) {
		return;
	}
	struct wlr_box newgeo = view->pending;
	newgeo.x -= left;
	newgeo.width += left + right;
	newgeo.y -= top;
	newgeo.height += top + bottom;
	view_move_resize(view, newgeo);
	view_set_untiled(view);
}

void
view_move_relative(struct view *view, int x, int y)
{
	if (view->fullscreen) {
		return;
	}
	view_maximize(view, false, /*store_natural_geometry*/ false);
	if (view_is_tiled(view)) {
		view_set_untiled(view);
		view_restore_to(view, view->natural_geometry);
	}
	view_move(view, view->pending.x + x, view->pending.y + y);
}

void
view_adjust_size(struct view *view, int *w, int *h)
{
	assert(view);

#if HAVE_XWAYLAND
	if (xwayland_apply_size_hints(view, w, h)) {
		/* We don't want to cap the size to keep the aspect ratio */
		return;
	}
#endif

	*w = MAX(*w, LAB_MIN_VIEW_WIDTH);
	*h = MAX(*h, LAB_MIN_VIEW_HEIGHT);
}

void
view_minimize(struct view *view, bool minimized)
{
	assert(view);
	if (view->minimized == minimized) {
		return;
	}
	if (view->toplevel.handle) {
		wlr_foreign_toplevel_handle_v1_set_minimized(
			view->toplevel.handle, minimized);
	}
	if (view->impl->minimize) {
		view->impl->minimize(view, minimized);
	}
	view->minimized = minimized;
	if (minimized) {
		view->impl->unmap(view, /* client_request */ false);
		_view_set_activated(view, false);
		if (view == view->server->focused_view) {
			/*
			 * Prevents clearing the active view when
			 * we don't actually have keyboard focus.
			 *
			 * This may happen when using a custom mouse
			 * focus configuration or by using the foreign
			 * toplevel protocol via some panel.
			 */
			view->server->focused_view = NULL;
		}
	} else {
		view->impl->map(view);
	}
}

static bool
view_compute_centered_position(struct view *view, const struct wlr_box *ref,
		int w, int h, int *x, int *y)
{
	if (w <= 0 || h <= 0) {
		wlr_log(WLR_ERROR, "view has empty geometry, not centering");
		return false;
	}
	if (!output_is_usable(view->output)) {
		wlr_log(WLR_ERROR, "view has no output, not centering");
		return false;
	}

	struct border margin = ssd_get_margin(view->ssd);
	struct wlr_box usable = output_usable_area_in_layout_coords(view->output);
	int width = w + margin.left + margin.right;
	int height = h + margin.top + margin.bottom;

	/* If reference box is NULL then center to usable area */
	if (!ref) {
		ref = &usable;
	}
	*x = ref->x + (ref->width - width) / 2;
	*y = ref->y + (ref->height - height) / 2;

	/* If view is bigger than usable area, just top/left align it */
	if (*x < usable.x) {
		*x = usable.x;
	}
	if (*y < usable.y) {
		*y = usable.y;
	}

	*x += margin.left;
	*y += margin.top;

	return true;
}

static void
set_fallback_geometry(struct view *view)
{
	view->natural_geometry.width = LAB_FALLBACK_WIDTH;
	view->natural_geometry.height = LAB_FALLBACK_HEIGHT;
	view_compute_centered_position(view, NULL,
		view->natural_geometry.width,
		view->natural_geometry.height,
		&view->natural_geometry.x,
		&view->natural_geometry.y);
}

#undef LAB_FALLBACK_WIDTH
#undef LAB_FALLBACK_HEIGHT

void
view_store_natural_geometry(struct view *view)
{
	assert(view);
	if (!view_is_floating(view)) {
		/* Do not overwrite the stored geometry with special cases */
		return;
	}

	/**
	 * If an application was started maximized or fullscreened, its
	 * natural_geometry width/height may still be zero in which case we set
	 * some fallback values. This is the case with foot and Qt applications.
	 */
	if (wlr_box_empty(&view->pending)) {
		set_fallback_geometry(view);
	} else {
		view->natural_geometry = view->pending;
	}
}

void
view_center(struct view *view, const struct wlr_box *ref)
{
	assert(view);
	int x, y;
	if (view_compute_centered_position(view, ref, view->pending.width,
			view->pending.height, &x, &y)) {
		view_move(view, x, y);
	}
}

static void
view_apply_natural_geometry(struct view *view)
{
	assert(view);
	assert(view_is_floating(view));

	struct wlr_output_layout *layout = view->server->output_layout;
	if (wlr_output_layout_intersects(layout, NULL, &view->natural_geometry)
			|| wl_list_empty(&layout->outputs)) {
		/* restore to original geometry */
		view_move_resize(view, view->natural_geometry);
	} else {
		/* reposition if original geometry is offscreen */
		struct wlr_box box = view->natural_geometry;
		if (view_compute_centered_position(view, NULL, box.width,
				box.height, &box.x, &box.y)) {
			view_move_resize(view, box);
		}
	}
}

static void
view_apply_region_geometry(struct view *view)
{
	assert(view);
	assert(view->tiled_region || view->tiled_region_evacuate);
	struct output *output = view->output;
	assert(output_is_usable(output));

	if (view->tiled_region_evacuate) {
		/* View was evacuated from a destroying output */
		/* Get new output local region, may be NULL */
		view->tiled_region = regions_from_name(
			view->tiled_region_evacuate, output);

		/* Get rid of the evacuate instruction */
		zfree(view->tiled_region_evacuate);

		if (!view->tiled_region) {
			/* Existing region name doesn't exist in rc.xml anymore */
			view_set_untiled(view);
			view_apply_natural_geometry(view);
			return;
		}
	}

	/* Create a copy of the original region geometry */
	struct wlr_box geo = view->tiled_region->geo;

	/* Adjust for rc.gap */
	if (rc.gap) {
		double half_gap = rc.gap / 2.0;
		struct wlr_fbox offset = {
			.x = half_gap,
			.y = half_gap,
			.width = -rc.gap,
			.height = -rc.gap
		};
		struct wlr_box usable =
			output_usable_area_in_layout_coords(output);
		if (geo.x == usable.x) {
			offset.x += half_gap;
			offset.width -= half_gap;
		}
		if (geo.y == usable.y) {
			offset.y += half_gap;
			offset.height -= half_gap;
		}
		if (geo.x + geo.width == usable.x + usable.width) {
			offset.width -= half_gap;
		}
		if (geo.y + geo.height == usable.y + usable.height) {
			offset.height -= half_gap;
		}
		geo.x += offset.x;
		geo.y += offset.y;
		geo.width += offset.width;
		geo.height += offset.height;
	}

	/* And adjust for current view */
	struct border margin = ssd_get_margin(view->ssd);
	geo.x += margin.left;
	geo.y += margin.top;
	geo.width -= margin.left + margin.right;
	geo.height -= margin.top + margin.bottom;

	view_move_resize(view, geo);
}

static void
view_apply_tiled_geometry(struct view *view)
{
	assert(view);
	assert(view->tiled);
	assert(output_is_usable(view->output));

	view_move_resize(view, view_get_edge_snap_box(view,
		view->output, view->tiled));
}

static void
view_apply_fullscreen_geometry(struct view *view)
{
	assert(view);
	assert(view->fullscreen);
	assert(output_is_usable(view->output));

	struct wlr_box box = { 0 };
	wlr_output_effective_resolution(view->output->wlr_output,
		&box.width, &box.height);
	double ox = 0, oy = 0;
	wlr_output_layout_output_coords(view->server->output_layout,
		view->output->wlr_output, &ox, &oy);
	box.x -= ox;
	box.y -= oy;
	view_move_resize(view, box);
}

static void
view_apply_maximized_geometry(struct view *view)
{
	assert(view);
	assert(view->maximized);
	struct output *output = view->output;
	assert(output_is_usable(output));

	struct wlr_box box = output_usable_area_in_layout_coords(output);
	if (box.height == output->wlr_output->height
			&& output->wlr_output->scale != 1) {
		box.height /= output->wlr_output->scale;
	}
	if (box.width == output->wlr_output->width
			&& output->wlr_output->scale != 1) {
		box.width /= output->wlr_output->scale;
	}

	if (view->ssd_enabled) {
		struct border border = ssd_thickness(view);
		box.x += border.left;
		box.y += border.top;
		box.width -= border.right + border.left;
		box.height -= border.top + border.bottom;
	}
	view_move_resize(view, box);
}

static void
view_apply_special_geometry(struct view *view)
{
	assert(view);
	assert(!view_is_floating(view));
	if (!output_is_usable(view->output)) {
		wlr_log(WLR_ERROR, "view has no output, not updating geometry");
		return;
	}

	if (view->fullscreen) {
		view_apply_fullscreen_geometry(view);
	} else if (view->maximized) {
		view_apply_maximized_geometry(view);
	} else if (view->tiled) {
		view_apply_tiled_geometry(view);
	} else if (view->tiled_region || view->tiled_region_evacuate) {
		view_apply_region_geometry(view);
	} else {
		assert(false); // not reached
	}
}

/* For internal use only. Does not update geometry. */
static void
set_maximized(struct view *view, bool maximized)
{
	if (view->impl->maximize) {
		view->impl->maximize(view, maximized);
	}
	if (view->toplevel.handle) {
		wlr_foreign_toplevel_handle_v1_set_maximized(
			view->toplevel.handle, maximized);
	}
	view->maximized = maximized;
}

/*
 * Un-maximize view and move it to specific geometry. Does not reset
 * tiled state (use view_set_untiled() if you want that).
 */
void
view_restore_to(struct view *view, struct wlr_box geometry)
{
	assert(view);
	if (view->fullscreen) {
		return;
	}
	if (view->maximized) {
		set_maximized(view, false);
	}
	view_move_resize(view, geometry);
}

bool
view_is_tiled(struct view *view)
{
	return view && (view->tiled || view->tiled_region
		|| view->tiled_region_evacuate);
}

bool
view_is_floating(struct view *view)
{
	return view && !(view->fullscreen || view->maximized || view->tiled
		|| view->tiled_region || view->tiled_region_evacuate);
}

/* Reset tiled state of view without changing geometry */
void
view_set_untiled(struct view *view)
{
	assert(view);
	view->tiled = VIEW_EDGE_INVALID;
	view->tiled_region = NULL;
	zfree(view->tiled_region_evacuate);
}

void
view_maximize(struct view *view, bool maximize, bool store_natural_geometry)
{
	assert(view);
	if (view->maximized == maximize) {
		return;
	}
	if (view->fullscreen) {
		return;
	}
	if (maximize) {
		/*
		 * Maximize via keybind or client request cancels
		 * interactive move/resize since we can't move/resize
		 * a maximized view.
		 */
		interactive_cancel(view);
		if (store_natural_geometry) {
			view_store_natural_geometry(view);
		}
	}
	set_maximized(view, maximize);
	if (view_is_floating(view)) {
		view_apply_natural_geometry(view);
	} else {
		view_apply_special_geometry(view);
	}
}

void
view_toggle_maximize(struct view *view)
{
	assert(view);
	view_maximize(view, !view->maximized,
		/*store_natural_geometry*/ true);
}

void
view_toggle_decorations(struct view *view)
{
	assert(view);
	view_set_decorations(view, !view->ssd_enabled);
}

bool
view_is_always_on_top(struct view *view)
{
	assert(view);
	return view->scene_tree->node.parent ==
		view->server->view_tree_always_on_top;
}

void
view_toggle_always_on_top(struct view *view)
{
	assert(view);
	if (view_is_always_on_top(view)) {
		view->workspace = view->server->workspace_current;
		wlr_scene_node_reparent(&view->scene_tree->node,
			view->workspace->tree);
	} else {
		wlr_scene_node_reparent(&view->scene_tree->node,
			view->server->view_tree_always_on_top);
	}
}

static bool
view_is_always_on_bottom(struct view *view)
{
	assert(view);
	return view->scene_tree->node.parent ==
		view->server->view_tree_always_on_bottom;
}

void
view_toggle_always_on_bottom(struct view *view)
{
	assert(view);
	if (view_is_always_on_bottom(view)) {
		view->workspace = view->server->workspace_current;
		wlr_scene_node_reparent(&view->scene_tree->node,
			view->workspace->tree);
	} else {
		wlr_scene_node_reparent(&view->scene_tree->node,
			view->server->view_tree_always_on_bottom);
	}
}

void
view_move_to_workspace(struct view *view, struct workspace *workspace)
{
	assert(view);
	assert(workspace);
	if (view->workspace != workspace) {
		view->workspace = workspace;
		wlr_scene_node_reparent(&view->scene_tree->node,
			workspace->tree);
	}
}

static void
decorate(struct view *view)
{
	if (!view->ssd) {
		view->ssd = ssd_create(view,
			view == view->server->focused_view);
	}
}

static void
undecorate(struct view *view)
{
	ssd_destroy(view->ssd);
	view->ssd = NULL;
}

void
view_set_decorations(struct view *view, bool decorations)
{
	assert(view);

	if (view->ssd_enabled != decorations && !view->fullscreen) {
		/*
		 * Set view->ssd_enabled first since it is referenced
		 * within the call tree of ssd_create()
		 */
		view->ssd_enabled = decorations;
		if (decorations) {
			decorate(view);
		} else {
			undecorate(view);
		}
		if (!view_is_floating(view)) {
			view_apply_special_geometry(view);
		}
	}
}

void
view_toggle_fullscreen(struct view *view)
{
	assert(view);
	view_set_fullscreen(view, !view->fullscreen);
}

/* For internal use only. Does not update geometry. */
static void
set_fullscreen(struct view *view, bool fullscreen)
{
	/* Hide decorations when going fullscreen */
	if (fullscreen && view->ssd_enabled) {
		undecorate(view);
	}

	if (view->impl->set_fullscreen) {
		view->impl->set_fullscreen(view, fullscreen);
	}
	if (view->toplevel.handle) {
		wlr_foreign_toplevel_handle_v1_set_fullscreen(
			view->toplevel.handle, fullscreen);
	}
	view->fullscreen = fullscreen;

	/* Re-show decorations when no longer fullscreen */
	if (!fullscreen && view->ssd_enabled) {
		decorate(view);
	}

	/* Show fullscreen views above top-layer */
	if (view->output) {
		uint32_t top = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
		wlr_scene_node_set_enabled(&view->output->layer_tree[top]->node,
			!fullscreen);
	}
}

void
view_set_fullscreen(struct view *view, bool fullscreen)
{
	assert(view);
	if (fullscreen == view->fullscreen) {
		return;
	}
	if (fullscreen) {
		if (!output_is_usable(view->output)) {
			/* Prevent fullscreen with no available outputs */
			return;
		}
		/*
		 * Fullscreen via keybind or client request cancels
		 * interactive move/resize since we can't move/resize
		 * a fullscreen view.
		 */
		interactive_cancel(view);
		view_store_natural_geometry(view);
	}

	set_fullscreen(view, fullscreen);
	if (view_is_floating(view)) {
		view_apply_natural_geometry(view);
	} else {
		view_apply_special_geometry(view);
	}
}

void
view_adjust_for_layout_change(struct view *view)
{
	assert(view);

	/* Exit fullscreen if output is lost */
	bool was_fullscreen = view->fullscreen;
	if (was_fullscreen && !output_is_usable(view->output)) {
		set_fullscreen(view, false);
	}

	/*
	 * Floating views are always assigned to the nearest output.
	 * Maximized/tiled views remain on the same output if possible.
	 */
	bool is_floating = view_is_floating(view);
	if (is_floating || !output_is_usable(view->output)) {
		view_discover_output(view);
	}

	if (!is_floating) {
		view_apply_special_geometry(view);
	} else if (was_fullscreen) {
		view_apply_natural_geometry(view);
	} else {
		/* reposition view if it's offscreen */
		if (!wlr_output_layout_intersects(view->server->output_layout,
				NULL, &view->pending)) {
			view_center(view, NULL);
		}
	}
	if (view->toplevel.handle) {
		foreign_toplevel_update_outputs(view);
	}
}

void
view_evacuate_region(struct view *view)
{
	assert(view);
	assert(view->tiled_region);
	if (!view->tiled_region_evacuate) {
		view->tiled_region_evacuate = xstrdup(view->tiled_region->name);
	}
	view->tiled_region = NULL;
}

void
view_on_output_destroy(struct view *view)
{
	assert(view);
	/*
	 * This is the only time we modify view->output for a fullscreen
	 * view. We expect view_adjust_for_layout_change() to be called
	 * shortly afterward, which will exit fullscreen.
	 */
	view->output = NULL;
}

#define VIEW_GET_NEXT_EDGES(_view_near_def, _view_far_def, _backward)	\
	struct output *output = view->output;				\
	struct server *server = output->server;				\
	struct view *v;							\
	wl_list_for_each(v, &server->views, link) {			\
		if (v == view || v->output != output)			\
			continue;					\
		struct border margin = ssd_get_margin(v->ssd);		\
		int vn = (_view_near_def);				\
		int vf = (_view_far_def);				\
		if (_backward) {					\
			if (vf + far_gap  < point)			\
				f = MAX(f, vf + far_gap  - point);	\
			if (vn + near_gap < point)			\
				n = MAX(n, vn + near_gap - point);	\
		} else {						\
			if (vf + far_gap  > point)			\
				f = MIN(f, vf + far_gap  - point);	\
			if (vn + near_gap > point)			\
				n = MIN(n, vn + near_gap - point);	\
		}							\
	}


static void
_view_get_next_edges(struct view *view, const char *direction, int point,
		     int max, int near_gap, int far_gap, int *near, int *far)
{
	/*
	 * Returns near and far edges from all views, looking from
	 * point towards direction. Stops searching at max.
	 *
	 * Note that the width and height are ignored when looking for
	 * edges (left/right only considers x, while up/down only
	 * considers y). This is intentional as it allows to easily
	 * align your windows to a grid, defined by all other windows)
	 */
	int n = max, f = max;
	if (!strcasecmp(direction, "left")) {
		VIEW_GET_NEXT_EDGES(v->current.x + v->current.width + margin.right,
				    v->current.x - margin.left,
				    true);
	} else if (!strcasecmp(direction, "up")) {
		VIEW_GET_NEXT_EDGES(v->current.y + v->current.height + margin.bottom,
				    v->current.y - margin.top,
				    true);
	} else if (!strcasecmp(direction, "right")) {
		VIEW_GET_NEXT_EDGES(v->current.x - margin.left,
				    v->current.x + v->current.width + margin.right,
				    false);
	} else if (!strcasecmp(direction, "down")) {
		VIEW_GET_NEXT_EDGES(v->current.y - margin.top,
				    v->current.y + v->current.height + margin.bottom,
				    false);
	}
	*near = n; *far = f;
}

void
view_move_to_edge(struct view *view, const char *direction, const char *snap)
{
	assert(view);
	struct output *output = view->output;
	if (!output_is_usable(output)) {
		wlr_log(WLR_ERROR, "view has no output, not moving view");
		return;
	}
	if (!direction) {
		wlr_log(WLR_ERROR, "invalid direction, not moving view");
		return;
	}

	struct edge_manip_hints em = view_get_edge_manip_hints(view);
	int dx = 0, dy = 0;

	if(!strcasecmp(snap, "window")) {
		int near, far;
		if (!strcasecmp(direction, "left")) {
			// left edge to left/right edges
			_view_get_next_edges(view, direction,
				em.left_edge, em.left_max_dx, rc.gap, 0, &near, &far);
			dx = MAX(near, far);
		} else if (!strcasecmp(direction, "up")) {
			// top edge to top/bottom edges
			_view_get_next_edges(view, direction,
				em.top_edge, em.up_max_dy, rc.gap, 0, &near, &far);
			dy = MAX(near, far);
		} else if (!strcasecmp(direction, "right")) {
			// left edge to left/right edges
			_view_get_next_edges(view, direction,
				em.left_edge, em.right_max_dx, 0, rc.gap, &near, &far);
			dx = MIN(near, far);
			// right edge to left edge
			_view_get_next_edges(view, direction,
				em.right_edge, em.right_max_dx, -rc.gap, 0, &near, &far);
			dx = MIN(dx, near);
		} else if (!strcasecmp(direction, "down")) {
			// top edge to top/bottom edges
			_view_get_next_edges(view, direction,
				em.top_edge, em.down_max_dy, 0, rc.gap, &near, &far);
			dy = MIN(near, far);
			// bottom edge to top edge
			_view_get_next_edges(view, direction,
				em.bottom_edge, em.down_max_dy, -rc.gap, 0, &near, &far);
			dy = MIN(dy, near);
		} else {
			wlr_log(WLR_ERROR, "invalid direction: %s", direction);
			return;
		}
	}
	else if(!strcasecmp(snap, "screen")) {
		if (!strcasecmp(direction, "left")) {
			dx = em.left_max_dx;
		} else if (!strcasecmp(direction, "up")) {
			dy = em.up_max_dy;
		} else if (!strcasecmp(direction, "right")) {
			dx = em.right_max_dx;
		} else if (!strcasecmp(direction, "down")) {
			dy = em.down_max_dy;
		} else {
			wlr_log(WLR_ERROR, "invalid direction: %s", direction);
			return;
		}
	} else {
		wlr_log(WLR_ERROR, "invalid snap: %s", snap);
		return;
	}

	view_move(view,
		  dx ? view->current.x + dx : view->pending.x,
		  dy ? view->current.y + dy : view->pending.y);
}

void
view_grow_to_edge(struct view *view, const char *direction)
{
	assert(view);
	if (view->fullscreen || view->maximized)
		return;

	struct output *output = view->output;
	if (!output_is_usable(output)) {
		wlr_log(WLR_ERROR, "view has no output, not growing view");
		return;
	}
	if (!direction) {
		wlr_log(WLR_ERROR, "invalid direction, not growing view");
		return;
	}

	struct edge_manip_hints em = view_get_edge_manip_hints(view);
	struct wlr_box geo = view->pending;
	int dx = 0, dy = 0;
	int near, far;
	if (!strcasecmp(direction, "left")) {
		// left edge to left/right edges
		_view_get_next_edges(view, direction,
			em.left_edge, em.left_max_dx, rc.gap, 0, &near, &far);
		dx = MAX(near, far);
		geo.width += -dx;
		geo.x     += dx;
	} else if (!strcasecmp(direction, "up")) {
		// top edge to top/bottom edges
		_view_get_next_edges(view, direction,
			em.top_edge, em.up_max_dy, rc.gap, 0, &near, &far);
		dy = MAX(near, far);
		geo.height += -dy;
		geo.y      += dy;
	} else if (!strcasecmp(direction, "right")) {
		// right edge to left/right edges
		_view_get_next_edges(view, direction,
			em.right_edge, em.right_max_dx, -rc.gap, 0, &near, &far);
		dx = MIN(near, far);
		geo.width  += dx;
	} else if (!strcasecmp(direction, "down")) {
		// bottom edge to top/bottom edges
		_view_get_next_edges(view, direction,
			em.bottom_edge, em.down_max_dy, -rc.gap, 0, &near, &far);
		dy = MIN(near, far);
		geo.height += dy;
	} else {
		wlr_log(WLR_ERROR, "invalid direction: %s", direction);
		return;
	}

	view_move_resize(view, geo);
}

void
view_shrink_to_edge(struct view *view, const char *direction)
{
	assert(view);
	if (view->fullscreen || view->maximized)
		return;

	struct output *output = view->output;
	if (!output_is_usable(output)) {
		wlr_log(WLR_ERROR, "view has no output, not shrinking view");
		return;
	}
	if (!direction) {
		wlr_log(WLR_ERROR, "invalid direction, not shrinking view");
		return;
	}

	struct edge_manip_hints em = view_get_edge_manip_hints(view);
	struct wlr_box geo = view->pending;
	int max_dx = MIN(em.width_max_dx,  view->pending.width  / 2);
	int max_dy = MIN(em.height_max_dy, view->pending.height / 2);
	int dx = 0, dy = 0;
	int near, far;

	if (!strcasecmp(direction, "left")) {
		// right edge to left/right edges
		_view_get_next_edges(view, direction,
			em.right_edge, -max_dx, 0, -rc.gap, &near, &far);
		dx = MAX(near, far);
		geo.width += dx;
	} else if (!strcasecmp(direction, "up")) {
		// bottom edge to top/bottom edges
		_view_get_next_edges(view, direction,
			em.bottom_edge, -max_dy, 0, -rc.gap, &near, &far);
		dy = MAX(near, far);
		geo.height += dy;
	} else if (!strcasecmp(direction, "right")) {
		// left edge to left/right edges
		_view_get_next_edges(view, direction,
			em.left_edge, max_dx, 0, rc.gap, &near, &far);
		dx = MIN(near, far);
		geo.width -= dx;
		geo.x     += dx;
	} else if (!strcasecmp(direction, "down")) {
		// top edge to top/bottom edges
		_view_get_next_edges(view, direction,
			em.top_edge, max_dy, 0, rc.gap, &near, &far);
		dy = MIN(near, far);
		geo.height -= dy;
		geo.y      += dy;
	} else {
		wlr_log(WLR_ERROR, "invalid direction: %s", direction);
		return;
	}

	view_move_resize(view, geo);
}

static enum view_edge
view_edge_parse(const char *direction)
{
	if (!direction) {
		return VIEW_EDGE_INVALID;
	}
	if (!strcasecmp(direction, "left")) {
		return VIEW_EDGE_LEFT;
	} else if (!strcasecmp(direction, "up")) {
		return VIEW_EDGE_UP;
	} else if (!strcasecmp(direction, "right")) {
		return VIEW_EDGE_RIGHT;
	} else if (!strcasecmp(direction, "down")) {
		return VIEW_EDGE_DOWN;
	} else if (!strcasecmp(direction, "center")) {
		return VIEW_EDGE_CENTER;
	} else {
		return VIEW_EDGE_INVALID;
	}
}

void
view_snap_to_edge(struct view *view, const char *direction,
		bool store_natural_geometry)
{
	assert(view);
	if (view->fullscreen) {
		return;
	}
	struct output *output = view->output;
	if (!output_is_usable(output)) {
		wlr_log(WLR_ERROR, "view has no output, not snapping to edge");
		return;
	}
	enum view_edge edge = view_edge_parse(direction);
	if (edge == VIEW_EDGE_INVALID) {
		wlr_log(WLR_ERROR, "invalid edge, not snapping view");
		return;
	}

	if (view->tiled == edge && !view->maximized) {
		/* We are already tiled for this edge and thus should switch outputs */
		struct wlr_output *new_output = NULL;
		struct wlr_output *current_output = output->wlr_output;
		struct wlr_output_layout *layout = view->server->output_layout;
		switch (edge) {
		case VIEW_EDGE_LEFT:
			new_output = wlr_output_layout_adjacent_output(
				layout, WLR_DIRECTION_LEFT, current_output, 1, 0);
			break;
		case VIEW_EDGE_RIGHT:
			new_output = wlr_output_layout_adjacent_output(
				layout, WLR_DIRECTION_RIGHT, current_output, 1, 0);
			break;
		case VIEW_EDGE_UP:
			new_output = wlr_output_layout_adjacent_output(
				layout, WLR_DIRECTION_UP, current_output, 0, 1);
			break;
		case VIEW_EDGE_DOWN:
			new_output = wlr_output_layout_adjacent_output(
				layout, WLR_DIRECTION_DOWN, current_output, 0, 1);
			break;
		default:
			break;
		}
		if (new_output && new_output != current_output) {
			/* Move to next output */
			edge = view_edge_invert(edge);
			output = output_from_wlr_output(view->server, new_output);
			if (!output_is_usable(output)) {
				wlr_log(WLR_ERROR, "invalid output in layout");
				return;
			}
		} else {
			/*
			 * No more output to move to
			 *
			 * We re-apply the tiled geometry without changing any
			 * state because the window might have been moved away
			 * (and thus got untiled) and then snapped back to the
			 * original edge.
			 *
			 * TODO: The described pattern will cause another bug
			 *       in multi monitor setups: it will snap the
			 *       window to the inverted edge of the nearest
			 *       output. This is the desired behavior when
			 *       caused by a keybind but doesn't make sense
			 *       when caused by mouse movement.
			 */
			view_apply_tiled_geometry(view);
			return;
		}
	}

	if (view->maximized) {
		/* Unmaximize + keep using existing natural_geometry */
		view_maximize(view, false, /*store_natural_geometry*/ false);
	} else if (store_natural_geometry) {
		/* store current geometry as new natural_geometry */
		view_store_natural_geometry(view);
	}
	view_set_untiled(view);
	view_set_output(view, output);
	view->tiled = edge;
	view_apply_tiled_geometry(view);
}

void
view_snap_to_region(struct view *view, struct region *region,
		bool store_natural_geometry)
{
	assert(view);
	assert(region);
	if (view->fullscreen) {
		return;
	}
	/* view_apply_region_geometry() needs a usable output */
	if (!output_is_usable(view->output)) {
		wlr_log(WLR_ERROR, "view has no output, not snapping to region");
		return;
	}

	if (view->maximized) {
		/* Unmaximize + keep using existing natural_geometry */
		view_maximize(view, false, /*store_natural_geometry*/ false);
	} else if (store_natural_geometry) {
		/* store current geometry as new natural_geometry */
		view_store_natural_geometry(view);
	}
	view_set_untiled(view);
	view->tiled_region = region;
	view_apply_region_geometry(view);
}

void
view_move_to_front(struct view *view)
{
	assert(view);
	if (view->impl->move_to_front) {
		view->impl->move_to_front(view);
		cursor_update_focus(view->server);
	}
}

void
view_move_to_back(struct view *view)
{
	assert(view);
	if (view->impl->move_to_back) {
		view->impl->move_to_back(view);
		cursor_update_focus(view->server);
	}
}

const char *
view_get_string_prop(struct view *view, const char *prop)
{
	assert(view);
	assert(prop);
	if (view->impl->get_string_prop) {
		return view->impl->get_string_prop(view, prop);
	}
	return "";
}

void
view_update_title(struct view *view)
{
	assert(view);
	const char *title = view_get_string_prop(view, "title");
	if (!view->toplevel.handle || !title) {
		return;
	}
	ssd_update_title(view->ssd);
	wlr_foreign_toplevel_handle_v1_set_title(view->toplevel.handle, title);
}

void
view_update_app_id(struct view *view)
{
	assert(view);
	const char *app_id = view_get_string_prop(view, "app_id");
	if (!view->toplevel.handle || !app_id) {
		return;
	}
	wlr_foreign_toplevel_handle_v1_set_app_id(
		view->toplevel.handle, app_id);
}

void
view_reload_ssd(struct view *view)
{
	assert(view);
	if (view->ssd_enabled && !view->fullscreen) {
		undecorate(view);
		decorate(view);
	}
}

static void
inhibit_keybinds(struct view *view, bool inhibit)
{
	assert(view->inhibits_keybinds != inhibit);

	view->inhibits_keybinds = inhibit;
	if (inhibit) {
		view->server->seat.nr_inhibited_keybind_views++;
	} else {
		view->server->seat.nr_inhibited_keybind_views--;
	}

	if (view->ssd_enabled) {
		ssd_enable_keybind_inhibit_indicator(view->ssd, inhibit);
	}
}

bool
view_inhibits_keybinds(struct view *view)
{
	return view && view->inhibits_keybinds;
}

void
view_toggle_keybinds(struct view *view)
{
	assert(view);
	inhibit_keybinds(view, !view->inhibits_keybinds);
}

void
view_destroy(struct view *view)
{
	assert(view);
	struct server *server = view->server;
	bool need_cursor_update = false;

	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_minimize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->destroy.link);

	if (view->toplevel.handle) {
		wlr_foreign_toplevel_handle_v1_destroy(view->toplevel.handle);
	}

	if (server->grabbed_view == view) {
		/* Application got killed while moving around */
		server->input_mode = LAB_INPUT_STATE_PASSTHROUGH;
		server->grabbed_view = NULL;
		need_cursor_update = true;
		regions_hide_overlay(&server->seat);
	}

	if (server->focused_view == view) {
		server->focused_view = NULL;
		need_cursor_update = true;
	}

	if (server->seat.pressed.view == view) {
		seat_reset_pressed(&server->seat);
	}

	if (view->tiled_region_evacuate) {
		zfree(view->tiled_region_evacuate);
	}

	if (view->inhibits_keybinds) {
		view->inhibits_keybinds = false;
		server->seat.nr_inhibited_keybind_views--;
	}

	osd_on_view_destroy(view);
	undecorate(view);

	if (view->scene_tree) {
		wlr_scene_node_destroy(&view->scene_tree->node);
		view->scene_tree = NULL;
	}

	/*
	 * The layer-shell top-layer is disabled when an application is running
	 * in fullscreen mode, so if that's the case, we have to re-enable it
	 * here.
	 */
	if (view->fullscreen && view->output) {
		uint32_t top = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
		wlr_scene_node_set_enabled(&view->output->layer_tree[top]->node,
			true);
	}

	/* If we spawned a window menu, close it */
	if (server->menu_current
			&& server->menu_current->triggered_by_view == view) {
		menu_close_root(server);
	}

	/* Remove view from server->views */
	wl_list_remove(&view->link);
	free(view);

	if (need_cursor_update) {
		cursor_update_focus(server);
	}
}
