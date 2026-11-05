"""Read selected unpacked LAD8 code ranges from a live process.

This opens the process with query/read rights only. It performs no writes, thread
suspension, breakpoints, or code changes.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path


PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010

RANGES = (
    (0x000DDB00, 0x300, "draw_emit_DDB00"),
    (0x027E7800, 0x600, "async_dispatch_27E7800"),
    (0x027F6000, 0x1800, "pass_playback_27F6000"),
)

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.ReadProcessMemory.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
kernel32.ReadProcessMemory.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("pid", type=int)
    parser.add_argument("image_base", type=lambda value: int(value, 0))
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    process = kernel32.OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, args.pid
    )
    if not process:
        raise OSError(ctypes.get_last_error(), "OpenProcess failed")

    args.output.mkdir(parents=True, exist_ok=True)
    try:
        for rva, size, name in RANGES:
            buffer = ctypes.create_string_buffer(size)
            count = ctypes.c_size_t()
            address = args.image_base + rva
            ok = kernel32.ReadProcessMemory(
                process,
                ctypes.c_void_p(address),
                buffer,
                size,
                ctypes.byref(count),
            )
            if not ok or count.value != size:
                raise OSError(ctypes.get_last_error(), f"read failed at RVA 0x{rva:X}")
            path = args.output / f"{name}_rva_{rva:08X}.bin"
            path.write_bytes(buffer.raw)
            print(f"DUMP {path} rva=0x{rva:X} bytes={size}")
    finally:
        kernel32.CloseHandle(process)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
