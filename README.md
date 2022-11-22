# vkwc
A Vulkan-based wayland compositor with the ability to arbitrarily rotate and translate windows in 3D.

## Screenshot
![](https://raw.githubusercontent.com/cynic64/vkwc/main/screenshots/1.png)

## Video
<a href="http://www.youtube.com/watch?feature=player_embedded&v=1KfXl7QdMD8" target="_blank"><img src="http://img.youtube.com/vi/1KfXl7QdMD8/0.jpg" alt="Vulkan Wayland Compositor" width="800" height="600" border="10" /></a>

## Installation
NOTE: This is highly unstable software! It's mostly a testbed for me to try neat things in Vulkan, and in no way
a useful compositor.

Since people have asked, however, here are the dependencies I use to compile it on Gentoo:
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
