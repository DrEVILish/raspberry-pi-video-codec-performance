# cutepicfhddec as a standalone GStreamer plugin. See README.md in this folder, and ../../tools/plugin.mk for the targets.
PLUGIN := libgstcutepicfhd.so
SRCS := cfhd.c
PKGS := gstreamer-1.0 gstreamer-video-1.0 gstreamer-app-1.0
ELEMENT := cutepicfhddec
CINEFORM_SDK ?= ../cineform-sdk-aarch64/cineform-sdk
DEFS := -DCFHD_PLUGIN -I$(CINEFORM_SDK)/Common
include ../../tools/plugin.mk
