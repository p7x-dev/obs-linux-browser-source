# AUR packaging

Arch User Repository packaging for `obs-widget-browser`.

## Files

- `PKGBUILD` — builds the plugin from the upstream `v$pkgver` tag.
- `.SRCINFO` — generated metadata; must be committed to the AUR repository.

The package installs the plugin in the standard OBS system layout:

```
/usr/lib/obs-plugins/obs-widget-browser.so
/usr/share/obs/obs-plugins/obs-widget-browser/locale/*.ini
```

## Validate locally

`makepkg` must not be run as root.

```sh
cd aur
makepkg --verifysource
makepkg -si
```

Optional checks if `namcap` is installed:

```sh
namcap PKGBUILD
namcap obs-widget-browser-0.1.2-1-x86_64.pkg.tar.zst
```

## Publish to AUR

1. Create an AUR account and add your SSH public key at
   <https://aur.archlinux.org/> (My Account).
2. Clone the (initially empty) AUR repository:

   ```sh
   git clone ssh://aur@aur.archlinux.org/obs-widget-browser.git
   cd obs-widget-browser
   ```

3. Copy `PKGBUILD` and `.SRCINFO` from this directory, commit and push:

   ```sh
   cp /path/to/obs-browser-plugin/aur/PKGBUILD .
   cp /path/to/obs-browser-plugin/aur/.SRCINFO .
   git add PKGBUILD .SRCINFO
   git commit -m "Initial import: obs-widget-browser 0.1.2"
   git push
   ```

4. Verify that <https://aur.archlinux.org/packages/obs-widget-browser> appears.

## Updating after a release

1. Bump `pkgver` in `PKGBUILD` and set `pkgrel=1`.
2. Update the checksum:

   ```sh
   updpkgsums
   ```

3. Regenerate the metadata:

   ```sh
   makepkg --printsrcinfo > .SRCINFO
   ```

4. Commit both files to the AUR repository.
