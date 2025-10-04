# ComRem Compatable Patch for kraken
import typing
import pathlib

class Patch(typing.NamedTuple):
    label:   str
    offset:  int
    content: bytes

PATCHES: list[Patch] = [
    Patch(
        "void __cdecl _E17()",
        0x005FF000,
        b"\x00\xDF\x98\x00"
    ),
    Patch(
        "Init Kraken",
        0x0058DF00,
        b"\x55\x89\xE5\x83\xEC\x0C\x68\x94\xED\x9F\x00\xFF\x15\x8C\xE1\x98"
        b"\x00\x89\x45\xFC\x83\x7D\xFC\x00\x0F\x85\x0B\x00\x00\x00\x6A\xFF"
        b"\xFF\x15\xBC\xE3\x98\x00\x83\xC4\x04\x6A\x01\x8B\x45\xFC\x50\xFF"
        b"\x15\x90\xE1\x98\x00\x89\x45\xF8\x83\x7D\xF8\x00\x0F\x85\x0B\x00"
        b"\x00\x00\x6A\xFF\xFF\x15\xBC\xE3\x98\x00\x83\xC4\x04\x8B\x4D\xF8"
        b"\x89\x4D\xF4\x8B\x55\xFC\x52\xFF\x55\xF4\x83\xC4\x04\x89\xEC\x5D"
        b"\xC3\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC"
    ),
    Patch(
        "dllLibrary",
        0x005FED94,
        b"kraken.dll\x00\x00"
    ),
    Patch(
        "4GB LAA",
        0x15E,
        b"\x2E"
    ),
    Patch(
        "4MB Stack Reserve",
        0x1A8,
        b"\x00\x00\x40\x00"
    ),
]

EXE = pathlib.Path(r".\\hta.exe")
BAK = pathlib.Path(r".\\hta.dat")

with open(EXE, 'rb+') as stream:
    patch: Patch
    for patch in PATCHES:
        print(f"Apply: [0x{patch.offset:08X}] [0x{len(patch.content):04X}] {patch.label}")
        stream.seek(patch.offset)
        stream.write(patch.content)