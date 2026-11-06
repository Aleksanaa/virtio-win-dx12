# QEMU

QEMU's own SDL or GTK window draws without SPICE's blocking, so stock QEMU is smooth here. With GTK, yttrium's QEMU patch
[cf3bba5c26](https://github.com/arehnman/yttrium-qemu/commit/cf3bba5c26) ("ui: decouple GL presentation from
renderer blocking") still helps, so a hidden or minimized GTK window doesn't stall the guest. SDL2 doesn't need it.

The guest needs a virtio-gpu device with `venus`, `blob` and a `hostmem` window, and its memory must be a shared
memfd:

```
-machine q35,memory-backend=mem
-object memory-backend-memfd,id=mem,size=8G,share=on
-device virtio-vga-gl,venus=on,blob=on,hostmem=4G
-display sdl,gl=on            # or gtk,gl=on
```

- **SDL** is pixel-exact on a fractionally scaled Wayland desktop if QEMU's environment has
  `SDL_VIDEO_WAYLAND_SCALE_TO_DISPLAY=1`. SDL sizes the window in physical pixels and the guest's resolution matches
  it. The hardware cursor shows a bit large, as SDL scales it by the host's display scale on top of the guest's own.
  There is no menu bar; Ctrl+Alt+F toggles fullscreen and Ctrl+Alt+G releases the mouse.
- **GTK** has menus, but GTK 3 draws only at integer scales, so on a fractionally scaled display the compositor
  resamples the window and text looks slightly pixelated.

For lower input latency, use virtio input devices (`-device virtio-keyboard-pci -device virtio-tablet-pci`) in place
of the emulated PS/2 and USB ones (the guest needs virtio-win's `vioinput` driver).
