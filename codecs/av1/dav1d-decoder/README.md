# cutepidav1ddec: build files

This folder builds the `cutepidav1ddec` GStreamer plugin. **What it does, measurements and full instructions:** [`../README.md`](../README.md).

```sh
make            # build here
make check      # load it with gst-inspect-1.0
make install    # ~/.local/share/gstreamer-1.0/plugins (or: sudo make install-system)
```
