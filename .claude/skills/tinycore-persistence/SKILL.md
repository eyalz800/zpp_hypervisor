---
name: tinycore-persistence
description: Use when installing packages or changing configuration on the TinyCore target - it runs from RAM, so nothing survives a reboot unless it is deliberately backed up
---

# Making changes stick on the TinyCore target

Target: `tc@192.168.1.199`. **The whole system runs from RAM.** Every install,
every edited file, every SSH key is gone on reboot unless it is captured. This
is the single most surprising thing about the machine and it has already cost
time twice: once losing an installed SSH key, once concluding a package was
missing when it was installed all along.

## Where state actually lives

- **Packages** live in the tce store on the ESP, at
  `/mnt/nvme0n1p2/EFI/tc/tce`, pointed at by the kernel command line
  (`tce=UUID=.../EFI/tc/tce`). `optional/` holds the `.tcz` files;
  `onboot.lst` lists what gets loaded at boot.
- **Files** are captured by `filetool.sh` according to `/opt/.filetool.lst`,
  which currently contains just `opt` and `home`. Anything outside those two
  trees is **not** backed up, no matter what you do to it.

## The procedure

Install a package so it returns after a reboot:

```sh
tce-load -wi <package>        # downloads into the tce store and loads it
grep <package> /mnt/nvme0n1p2/EFI/tc/tce/onboot.lst   # confirm it will reload
```

Persist a file change:

```sh
# only works for paths under /opt or /home
cd ~/vm && ./backup.sh        # wraps filetool.sh -bv
```

To make something outside `/opt` or `/home` come back - a symlink in
`/usr/bin`, a module parameter - **do not back up the file**. Add the command
that recreates it to `/opt/bootlocal.sh`, which is under `/opt` and therefore
is captured, then run the backup.

## Traps

- **The ESP must be mounted for any of this.** The VFIO rig unmounts the NVMe
  partitions and rebinds the device to `vfio-pci`, so while a VM is running
  the tce store is unreachable and nothing can be installed or backed up.
  Mount it first with `sudo mount /dev/nvme0n1p2 /mnt/nvme0n1p2`.
- **`sudo` has its own `PATH`.** Packages commonly install to
  `/usr/local/sbin`, which is on the user's `PATH` but not in sudo's
  `secure_path`. A binary can be installed, loaded, listed in `onboot.lst`,
  and still produce `sudo: <tool>: command not found`. Symlink it into
  `/usr/bin` rather than reinstalling - `tce-load` will only tell you it is
  already installed, which reads as a contradiction and is not one.
- **SSH host keys regenerate every boot.** The changed-host-key warning is
  expected, not an attack. Clear it with `ssh-keygen -R 192.168.1.199` and
  connect with `-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null`.
- **The SSH authorized key is in `/home`,** so it does survive - but only if a
  backup was taken after installing it. If key auth stops working, the
  password is `1`.
- `~/.ash_history` is deleted by `backup.sh` before it runs, so shell history
  is not a place to look for what was done last session.

Related skill: `boot-windows-rig`.
