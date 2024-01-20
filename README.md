# vkwc
A Vulkan-based wayland compositor with the ability to arbitrarily rotate and translate windows in 3D and apply all kinds of weird effects.

## Screenshots
![](https://raw.githubusercontent.com/cynic64/vkwc/main/screenshots/1.png)

![](https://raw.githubusercontent.com/cynic64/vkwc/main/screenshots/2.png)

## Video of old version
<a href="http://www.youtube.com/watch?feature=player_embedded&v=1KfXl7QdMD8" target="_blank"><img src="http://img.youtube.com/vi/1KfXl7QdMD8/0.jpg" alt="Vulkan Wayland Compositor" width="800" height="600" border="10" /></a>

It used to have a physics simulation, but I took it out for now. I'll reimplement it eventually™ ;).

# Compiling
I wouldn't bother trying, this is a ridiculously hacky codebase that's meant more for me to experiment than anything else. If you really want to suffer, go ahead!

```
meson build/
ninja -C build
build/vkwc
```

Run it inside a compositor like Sway, otherwise it'll probably hang and you'll have to hard reboot your system.

I build it with wlroots 0.16.2.
