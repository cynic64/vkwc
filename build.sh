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

LIBS="\
        -lm -g
        $(pkg-config --cflags --libs wlroots) \
        $(pkg-config --cflags --libs wayland-server) \
        $(pkg-config --cflags --libs pixman-1) \
        $(pkg-config --cflags --libs vulkan) \
        $(pkg-config --cflags --libs xkbcommon)"

COMMAND="gcc -Wall -pedantic -ggdb -o vkwc vkwc.c render.c $LIBS -DWLR_USE_UNSTABLE -I."
echo "Command: $COMMAND"

$COMMAND
