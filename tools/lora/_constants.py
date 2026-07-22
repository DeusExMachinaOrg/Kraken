"""Shared constants for the lora MCP server."""

NSF_REGEX = 8  # DIA nsfRegularExpression flag

BASE_TYPE_NAMES = {
    0: "...", 1: "void", 2: "char", 3: "WCHAR", 6: "int",
    7: "unsigned int", 8: "float", 9: "BCD", 10: "bool",
    13: "long", 14: "unsigned long", 15: "CURRENCY", 16: "DATE",
    17: "VARIANT", 25: "HRESULT", 26: "char16_t", 27: "char32_t",
    28: "char8_t", 30: "__int64", 31: "unsigned __int64",
}

CC_NAMES = {
    0: "__cdecl", 1: "__far_cdecl", 2: "__pascal", 3: "__far_pascal",
    4: "__fastcall", 5: "__far_fastcall", 7: "__stdcall", 8: "__far_stdcall",
    11: "__thiscall",
}

REG_NAMES_X86 = {
    0: "none", 17: "eax", 18: "ecx", 19: "edx", 20: "ebx",
    21: "esp", 22: "ebp", 23: "esi", 24: "edi",
    9: "ax", 10: "cx", 11: "dx", 12: "bx",
    1: "al", 2: "cl", 3: "dl", 4: "bl",
    30006: "vframe",
}

LOC_NAMES = {
    0: "null", 1: "static", 2: "tls", 3: "regrel", 4: "thisrel",
    5: "enreg", 6: "bitfield", 7: "slot", 8: "ilrel",
    9: "metadata", 10: "constant",
}

DK_NAMES = {
    0: "unknown", 1: "local", 2: "static_local", 3: "param",
    4: "object_ptr", 5: "file_static", 6: "global",
    7: "member", 8: "static_member", 9: "constant",
}

ACCESS_NAMES = {1: "private", 2: "protected", 3: "public"}
