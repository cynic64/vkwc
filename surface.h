#ifndef surface_h_INCLUDED
#define surface_h_INCLUDED

#include <cglm/cglm.h>
#include <wlr/types/wlr_surface.h>

struct Surface {
	struct Server *server;
	struct wl_list link;
	struct wl_listener destroy;
	struct wlr_surface *wlr_surface;
	// Is a float since it gets written to the depth buffer
	float id;
	// This points to the Surface data associated with the "main window" a surface belongs to. So all the titlebars and such can easily access the surface data of the main window.
	struct Surface *toplevel;
	bool is_toplevel;

	mat4 matrix;
	int x, y, width, height;

	// Set these and calc_matrices will do the rest. All are in radians.
	double x_rot, y_rot, z_rot;
};

struct Surface *find_surface(struct wlr_surface *needle, struct wl_list *haystack);

#endif // surface_h_INCLUDED

