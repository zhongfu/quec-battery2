# Packaging details

This document describes how the OpenWrt package is built, and how it behaves on
the Mudi 7.

## Build

```sh
make clean
make CC=aarch64-linux-musl-gcc
make ipk
```

You can also call the build script directly:

```sh
VERSION=1.2.3 ARCH=aarch64_cortex-a53 ./packaging/build-ipk.sh
```

The build writes `dist/quec-battery2_<version>_<arch>.ipk`. The Mudi 7
architecture is `aarch64_cortex-a53`.

The `.ipk` is a gzip tar archive. It contains `debian-binary`,
`control.tar.gz`, and `data.tar.gz`. GNU tar is required.

A prebuilt aarch64 musl toolchain is available from musl.cc. The release
workflow uses `https://more.musl.cc/x86_64-linux-musl/aarch64-linux-musl-cross.tgz`.

## Package contents

- `/usr/bin/quec_battery2` — the replacement daemon.
- `preinst`, `postinst`, `prerm`, `postrm` — control scripts.

## Why the payload is `/usr/bin/quec_battery2`

On the Mudi 7, the vendor `quec_battery` package owns `/usr/bin/quec_battery`,
`/etc/init.d/charge.init`, and `/etc/config/qlbattery`. opkg rejects a second
package that ships the same path (`check_data_file_clashes`). If the package
declares `Conflicts: quec_battery`, opkg removes the vendor package and its init
script and config.

The package therefore installs `/usr/bin/quec_battery2`. It uses
`/usr/bin/quec_battery` as a symlink to the new binary. The vendor package stays
installed, so the stock init script and configuration keep working.

## Control scripts

Install:

- `preinst`: stop `charge.init`. If `/usr/bin/quec_battery.stock` does not
  exist, move `/usr/bin/quec_battery` to it.
- `postinst`: symlink `/usr/bin/quec_battery` to `quec_battery2`. Start
  `charge.init`.

Upgrade:

- `preinst`: keep the saved stock binary. The `.stock` guard prevents a move.
- `postrm`: do nothing. The new `postinst` recreates the symlink.

Removal:

- `prerm`: stop `charge.init`.
- `postrm`: remove the symlink. Move `/usr/bin/quec_battery.stock` back to
  `/usr/bin/quec_battery`. Start `charge.init`.

opkg runs the scripts in this order:

| Operation | Order |
| --- | --- |
| install | `preinst`, unpack, `postinst` |
| upgrade | `prerm`(upgrade), `preinst`(upgrade), `postrm`(upgrade), unpack, `postinst` |
| remove | `prerm`(remove), remove files, `postrm`(remove) |

Because `postrm` also runs during an upgrade, it must skip the restore step for
the `upgrade` argument. Otherwise it would delete the live symlink and consume
the saved stock binary.

The daemon ignores `SIGTERM`. `charge.init stop` returns before procd sends
`SIGKILL` (after its 5 s timeout). Each script that stops the daemon therefore
waits for the process to exit. Without this wait, a restart command is a no-op
and the old binary keeps running from a deleted file.

## Install and remove

```sh
opkg install dist/quec-battery2_<version>_aarch64_cortex-a53.ipk
opkg remove quec-battery2
```

Verified on a Mudi 7:

| Step | Result |
| --- | --- |
| install | `/usr/bin/quec_battery` is a symlink to `quec_battery2`; `.stock` holds the stock binary |
| upgrade | the symlink stays valid; `.stock` still holds the stock binary |
| remove | `.stock` moves back to `/usr/bin/quec_battery` as a regular file |

## Recovery

The factory files are in `/rom`. If a package step fails, copy them back:

```sh
cp /rom/usr/bin/quec_battery /usr/bin/quec_battery
cp /rom/etc/init.d/charge.init /etc/init.d/charge.init
cp /rom/etc/config/qlbattery /etc/config/qlbattery
```

If a package was removed by accident, re-add its stanza from
`/rom/usr/lib/opkg/status`, and copy `/rom/usr/lib/opkg/info/quec_battery.*`
back to `/usr/lib/opkg/info/`.

## CI

`.github/workflows/release.yml` runs the host behavior suite, cross-compiles for
aarch64 musl, builds the `.ipk`, and uploads the binary and package as workflow
artifacts. On a `v*` tag, it publishes both to the GitHub release.
