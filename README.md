# vkwc
A Vulkan-based wayland compositor with the ability to arbitrarily rotate and translate windows in 3D.

## Screenshot
![](https://raw.githubusercontent.com/cynic64/vkwc/main/screenshots/1.png)

## Video
<a href="http://www.youtube.com/watch?feature=player_embedded&v=1KfXl7QdMD8" target="_blank"><img src="http://img.youtube.com/vi/1KfXl7QdMD8/0.jpg" alt="Vulkan Wayland Compositor" width="800" height="600" border="10" /></a>

## Installation
NOTE: This is highly unstable software! I really don't recommend trying it yet.

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
```

Additionally, you'll need to install [cglm](https://github.com/recp/cglm), which isn't available in the Gentoo repos.

Then run `$ ./build.sh` and pray. An executable called `vkwc` should be created in the same directory.

Feel free to add an issue if this doesn't work, which it probably won't. I may not be able to help you though.

## Usage
I **highly** recommend running vkwc nested a more stable compositor like Sway. If you run this in its own TTY, it may
hang up and you will have to force-shutdown your device.

- Alt+F2 runs `foot`, a Wayland-native terminal emulator
- Alt+Esc exits
- New windows spawn roughly where the mouse is - use this if you want to stack things in a particular way

By default, a physics simulation is connected to the compositor, so windows should pile on top of each other somewhat
realistically.

Good luck!
