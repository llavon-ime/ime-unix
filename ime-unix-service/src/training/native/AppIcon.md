# App icon

`AppIcon-source.png` is the original 460×460 sunglasses alpaca artwork from
[the project's website](https://github.com/llavon-ime/llavon-web/blob/main/public/llavon-avatar.png).
It is the same artwork used by the earlier personalization manager, whose
embedded version was only 96×96.

`AppIcon.png` is the prepared 1024×1024 RGBA app icon. The artwork fills a centered
824×824 rounded silhouette with a transparent 100-pixel outer margin. The image
and white surface share a clipping path, eliminating the hard square edge and
second gray plate visible when macOS wrapped the unmasked original. Corners are
antialiased and the original composition is retained.

Regenerate it on macOS with:

```sh
swift macos/scripts/render-app-icon.swift \
  ime-unix-service/src/training/native/AppIcon-source.png \
  ime-unix-service/src/training/native/AppIcon.png
```

The native manager embeds this image for its Linux window icon and installs it
for the desktop launcher. On macOS, `build-app-icon.cmake` generates the
multi-resolution `AppIcon.icns` bundled with the app from this same image.
The macOS input-method bundle also uses this image through
`macos/scripts/build-native-app.sh`; the earlier blue character icon is removed.
