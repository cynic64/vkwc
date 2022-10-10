#include <stdio.h>
#include <stdlib.h>
#include <wayland-server-core.h>

#include "surface.h"

// Find a Surface given a corresponding wlr_surface and a wl_list of Surfaces. Great naming, I know.
// Dies on failure.
struct Surface *find_surface(struct wlr_surface *needle, struct wl_list *haystack) {
	struct Surface *cur;
	wl_list_for_each(cur, haystack, link) {
		if (cur->wlr_surface == needle) {
			return cur;
		}
	}

	fprintf(stderr, "Couldn't find surface. Looked through %d using needle %p\n",
		wl_list_length(haystack), (void *) needle);
	exit(1);
}

