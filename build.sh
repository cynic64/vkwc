#!/bin/bash

# First build the XDG stuff
WAYLAND_SCANNER=$(pkg-config --variable=wayland_scanner wayland-scanner)
WAYLAND_PROTOCOLS=$(pkg-config --variable=pkgdatadir wayland-protocols)

COMMAND="$WAYLAND_SCANNER server-header \
        $WAYLAND_PROTOCOLS/stable/xdg-shell/xdg-shell.xml xdg-shell-protocol.h"
echo "XDG shell protocol header command: $COMMAND"
$COMMAND

COMMAND="$WAYLAND_SCANNER private-code \
        $WAYLAND_PROTOCOLS/stable/xdg-shell/xdg-shell.xml xdg-shell-protocol.c"
echo "XDG shell protocol .c command: $COMMAND"
$COMMAND

for shader in vulkan/shaders/*
do
	vn=$(basename $shader | sed 's/\./_/g')"_data"
	COMMAND="glslangValidator -V $shader -o render/$shader.h --vn $vn"
	echo $COMMAND
	$COMMAND
done

LIBS="\
        -g
        $(pkg-config --cflags --libs wlroots) \
        $(pkg-config --cflags --libs wayland-server) \
        $(pkg-config --cflags --libs pixman-1) \
        $(pkg-config --cflags --libs vulkan) \
        $(pkg-config --cflags --libs xkbcommon)"

COMMAND="gcc -lm -Wall -pedantic -ggdb -o vkwc vkwc.c vulkan/*.c render.c util.c surface.c misc/pixel_format.c $LIBS -lraylib -DWLR_USE_UNSTABLE -I."
echo "Command: $COMMAND"
$COMMAND

