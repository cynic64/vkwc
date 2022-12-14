# vkwc
A Vulkan-based wayland compositor with the ability to arbitrarily rotate and translate windows in 3D.

## Screenshot
![](https://raw.githubusercontent.com/cynic64/vkwc/main/screenshots/1.png)

## Video
<a href="http://www.youtube.com/watch?feature=player_embedded&v=1KfXl7QdMD8" target="_blank"><img src="http://img.youtube.com/vi/1KfXl7QdMD8/0.jpg" alt="Vulkan Wayland Compositor" width="800" height="600" border="10" /></a>

## Dependencies
NOTE: This is highly unstable software! I really don't recommend trying it yet.

- The physics simulation is incredibly resource-intensive and may bog down your machine
- Xwayland is only partially supported
- The terminal emulator is hard-coded to `foot`
- Only one monitor is supported
- Spawning more than three windows will grind the physics sim to a halt
- Resizing and moving is disabled to give the physics sim control

A few people have asked anyway, however, so here are the dependencies I use to compile it on Gentoo:
```
dev-libs/wayland-1.21.0
dev-libs/wayland-protocols-1.28
x11-base/xwayland-22.1.5
dev-util/vulkan-headers-1.3.224
dev-util/vulkan-tools-1.3.224
media-libs/vulkan-layers-1.3.224
media-libs/vulkan-loader-1.3.224
gui-libs/wlroots-0.15.1-r1
dev-util/glslang-1.3.224-r1
gui-apps/foot-1.13.1
```

Additionally, you'll need to install [cglm](https://github.com/recp/cglm), which isn't available in the Gentoo repos.
You'll also need meson and ninja.

## Building
```
$ meson build/
$ ninja -C build
```

An executable called `vkwc` will be created in `build/`. Feel free to raise an issue if this doesn't work.

## Usage
I **highly** recommend running vkwc nested a more stable compositor like Sway. If you run this in its own TTY, it may
hang up and you will have to force-shutdown your device.

- Use `./vkwc -s <program>` to execute `program` upon startup
- Alt+Esc exits
- Alt+F2 runs `foot`, a Wayland-native terminal emulator
- Alt+R cycles between displaying three buffers: color, depth and UV
- New windows spawn centered where the mouse is - use this if you want to stack things in a particular way

By default, a physics simulation is connected to the compositor, so windows should pile on top of each other somewhat
realistically.

Good luck!

