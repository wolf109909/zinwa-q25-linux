# phoc-q25

Arch Linux ARM's `phoc` 0.57 is linked against the stock system `wlroots0.20`.
Phoc upstream builds against its *embedded* wlroots with one phoc-specific patch,
"Revert layer-shell: error on 0 dimension without anchors". Without it, Phosh's
home bar (a bottom/left/right-anchored layer surface with height 0) is rejected:

    zwlr_layer_surface_v1#NN: error 2: height 0 requested without setting top and bottom anchors
    phosh: Error 71 (Protocol error) dispatching to Wayland display.

and Phosh crash-loops. This package builds phoc as upstream intends (static,
patched, embedded wlroots) and `provides`/`conflicts` `phoc`. `phoc` and `phosh`
are held in `/etc/pacman.conf` so they move together; rebuild this package when
bumping them.
