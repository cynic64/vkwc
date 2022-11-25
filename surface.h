#ifndef surface_h_INCLUDED
#define surface_h_INCLUDED

#include <cglm/cglm.h>
#include <wlr/types/wlr_surface.h>

#ifndef PHYSAC_IMPLEMENTATION
#define PHYSAC_STANDALONE
#include "physac.h"
#endif

struct Surface {
	struct wl_list link;
	struct wl_listener destroy;
	struct wlr_surface *wlr_surface;
	struct wlr_xdg_surface *xdg_surface;
	
	float id;			// Is a float since it gets written to the depth buffer
	
	struct Surface *toplevel;	// This points to the Surface data associated with the "main window" a
					// surface belongs to. So all the titlebars and such can easily access the
					// surface data of the main window.
					// If this _is_ the toplevel surface, set it to point to itself.

	mat4 matrix;
	int width, height;

	// Set these and calc_matrices will do the rest. Rotations in radians, speeds in radians per frame.
	float x, y, z;
	double x_rot, y_rot, z_rot;
	double x_rot_speed, y_rot_speed, z_rot_speed;

	PhysicsBody body;
	// Physics will only be applied to the surface if this is enabled
	bool apply_physics;
};

struct Surface *find_surface(struct wlr_surface *needle, struct wl_list *haystack);

#endif // surface_h_INCLUDED

