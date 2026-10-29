# Windows 98 SE export tables

Used by `../check9x.sh`, which fails the build if the drop-in imports anything Windows 98 can't
provide. One file per system DLL: the names that DLL exports, one per line.

**Source.** The DLLs in `C:\WINDOWS\SYSTEM` of a real Windows 98 SE install (QuickInstall = stock
98 SE + DirectX 6.1a) in the 86Box VM, read from its disk image on 2026-09-30:

    7z x <vm>.vhd, then objdump -p on each DLL (the [Ordinal/Name Pointer] table)

Only the names are kept here, not Microsoft's files.

**ALLOWED_W.txt.** Windows 9x exports most system "W" (Unicode) functions only as stubs that fail
at run time. The checker rejects every imported `...W` function from KERNEL32/USER32/GDI32/ADVAPI32
unless it is listed here, meaning Windows 9x really implements it. Anything only used on NT is
looked up with GetProcAddress instead of imported.

**Windows 95.** Not checked against real files (no install to read). What is known is handled in
the code: Winsock 1.1 only (the drop-in imports `wsock32.dll`, `WSAIoctl` is looked up), no
`GetFileAttributesEx`, no `CP_UTF8`, no iphlpapi, CryptoAPI only from OSR2. msvcrt.dll must be
present (IE4 and many games install it). A 95 install would let these tables be added for it.
