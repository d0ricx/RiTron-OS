# Shell and FAT16 filesystem

## Volume layout and persistence

The QEMU data disk is a 16 MiB FAT16 image at `build/data-disk.img`. The shell
mounts it behind a virtual root:

```text
/ROOT/
  SYSTEM/    # BOOTX64.EFI and KERNEL.EFI; read-only through the shell
  USR/       # writable user files; initial working directory
```

`make disk` updates the system EFI files and preserves existing user data.
The image is persistent across QEMU runs. Back it up before using `make clean`,
which removes the entire `build/` directory.

The filesystem supports printable ASCII long filenames up to 255 characters,
case-insensitive lookup, and FAT short-name aliases. Non-ASCII names are not
supported. It uses FAT16 and is not journaled.

## Shell commands

At the `>> ` prompt, use `help` to display the command list.

| Command | Description |
| --- | --- |
| `help` | Show available commands. |
| `ls` | List the current directory. |
| `pwd` | Print the current path. |
| `cd <path>` | Change directory; `cd ..` moves up and `cd /` returns to `/ROOT`. |
| `mkdir <path>` | Create a directory, including nested paths. |
| `rmdir <path>` | Remove an empty directory. |
| `rmdir -rf <path>` | Recursively remove a directory and its contents. |
| `mkfile <path>` | Create an empty file. |
| `cat <file>` | Print a text file (up to 64 KiB). |
| `open <file>` | Open a file in the text editor, or start a new unsaved file. |
| `cp <source> <destination>` | Copy a regular file to a new path. |
| `mv <source> <destination>` | Move or rename a regular file to a new path. |
| `rm <file>` | Remove a file. |
| `ip` | Show the e1000 MAC and DHCP network configuration. |
| `httpget <IPv4[:port]> <path> [save-as]` | Fetch plain HTTP and display or save the response. |
| `quit` | Request QEMU-compatible poweroff. |

Paths may be relative or absolute and accept `/` or `\` separators. Quote
arguments containing spaces:

```text
mkdir NOTES
open "NOTES/TODO LIST.TXT"
cat "NOTES/TODO LIST.TXT"
cp "NOTES/TODO LIST.TXT" "TODO BACKUP.TXT"
```

`cp` and `mv` operate on regular files and refuse to overwrite existing
entries. The shell supports Up/Down command history (eight recent commands),
cursor movement and editing, and Tab completion for commands and names in the
current directory. Input is limited to 599 characters.

## Text editor

The editor supports printable ASCII text and files up to 64 KiB. Enter adds a
newline; Backspace/Delete remove text; arrow keys move the cursor; Home and End
move to the line boundaries. CRLF and CR endings are normalized to LF when
opening a file.

- **Ctrl+S** saves the file.
- **Ctrl+X** closes the editor and discards unsaved changes.
- **Tab** inserts a tab.

Files in `/ROOT/SYSTEM` cannot be edited or saved.

## Storage behavior and limitations

Replacing a file stages and flushes its new contents and FAT chain before
switching the directory entry, then releases the old chain. A move creates and
flushes the destination before removing the source. These orderings reduce the
chance of losing file contents on write failures, but FAT16 has no journal:
power loss during a metadata update can still leave orphaned clusters or
damaged metadata.

`rmdir -rf` permanently removes the named directory and contents from the
virtual disk; it refuses to remove `/ROOT`, `/ROOT/SYSTEM`, or the current
directory and its parents. The data disk has finite capacity, and ATA support
is currently limited to the project's legacy 28-bit LBA PIO disk.
