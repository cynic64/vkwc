#ifndef surface_h_INCLUDED
#define surface_h_INCLUDED

#include <cglm/cglm.h>
#include <wlr/types/wlr_surface.h>

#ifndef PHYSAC_IMPLEMENTATION
#define PHYSAC_STANDALONE
#include "physac.h"
#endif

struct Surface {
	struct Server *server;
	struct wl_list link;
	struct wl_listener destroy;
	struct wlr_surface *wlr_surface;
	struct xdg_surface *xdg_surface;
	
	float id;			// Is a float since it gets written to the depth buffer
	
	struct Surface *toplevel;	// This points to the Surface data associated with the "main window" a
					// surface belongs to. So all the titlebars and such can easily access the
					// surface data of the main window.
	bool is_toplevel;

	mat4 matrix;
	int x, y, width, height;

	// Set these and calc_matrices will do the rest. Rotations in radians, speeds in radians per frame.
	double x_rot, y_rot, z_rot;
	double x_rot_speed, y_rot_speed, z_rot_speed;

	double x_offset, y_offset, z_offset;	// These get added to the scene node position

	PhysicsBody body;
	bool apply_physics;			// Physics will only be applied to the surface if this is enabled
};

struct Surface *find_surface(struct wlr_surface *needle, struct wl_list *haystack);

#endif // surface_h_INCLUDED

