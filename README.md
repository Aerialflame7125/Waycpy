# Waycpy

Waycpy = Wayland Copy, copies screen memory buffer of a virtual monitor and drops it into a drm instance for another GPU
(also my first C project)

An AI generated project to fix a really niche problem.

Creates virtual outputs for either Sway or Hyprland based upon an (unowned) Gpu handle from /dev/dri.

## Usage
./drmtest /dev/dri/card{x}

* This will create a new DRM instance for the card and iterate through all displays, creating separate virtual displays and drawing the virtual display output to said physical displays.
