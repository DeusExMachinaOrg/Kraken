"""DIA/PDB operations: symbol queries, type formatting, prototypes."""

from __future__ import annotations

import re
from typing import Any

from pydia2 import cvconst

from ._constants import BASE_TYPE_NAMES, CC_NAMES, NSF_REGEX
from ._state import Session


# ── Type formatting ──────────────────────────────────────────────────────


def type_name(sym) -> str:
    """Best-effort human-readable type name from a DIA type symbol."""
    try:
        tag = int(sym.symTag)
    except Exception:
        return "?"

    if tag == int(cvconst.SymTag.BaseType):
        name = BASE_TYPE_NAMES.get(int(sym.baseType), f"base({int(sym.baseType)})")
        length = int(sym.length)
        if name == "int" and length == 1:
            return "int8_t"
        if name == "int" and length == 2:
            return "int16_t"
        if name == "unsigned int" and length == 1:
            return "uint8_t"
        if name == "unsigned int" and length == 2:
            return "uint16_t"
        return name

    if tag == int(cvconst.SymTag.PointerType):
        try:
            return type_name(sym.type) + "*"
        except Exception:
            return "ptr"

    if tag == int(cvconst.SymTag.ArrayType):
        try:
            return f"{type_name(sym.type)}[{int(sym.count)}]"
        except Exception:
            return "array"

    try:
        if sym.name:
            return sym.name
    except Exception:
        pass

    return sym_tag_name(tag)


def sym_tag_name(tag_val: int) -> str:
    for name, member in vars(cvconst.SymTag).items():
        if not name.startswith("_") and int(member) == tag_val:
            return name
    return str(tag_val)


# ── Symbol resolution ────────────────────────────────────────────────────


def sym_at_rva(sess: Session, rva: int) -> str | None:
    """Resolve RVA to PDB symbol name, with caching."""
    if rva in sess.sym_cache:
        return sess.sym_cache[rva]
    result = None
    try:
        sym = sess.session.findSymbolByRVA(rva, cvconst.SymTag.Null)
        if sym and sym.name:
            actual = int(sym.relativeVirtualAddress)
            offset = rva - actual
            if offset == 0:
                result = sym.name
            elif offset <= 0x10000:
                result = f"{sym.name}+0x{offset:X}"
    except Exception:
        pass
    sess.sym_cache[rva] = result
    return result


def func_parent_class(sess: Session, rva: int) -> str | None:
    """Get the parent class name of a function at RVA."""
    try:
        sym = sess.session.findSymbolByRVA(rva, cvconst.SymTag.Function)
        if sym and sym.name:
            idx = sym.name.rfind("::")
            if idx > 0:
                return sym.name[:idx]
    except Exception:
        pass
    return None


def resolve_function(sess: Session, name_or_rva: str) -> tuple[int, int, str]:
    """Resolve a function by name or RVA hex. Returns (rva, length, name)."""
    if name_or_rva.startswith("0x") or name_or_rva.startswith("0X"):
        func_rva = int(name_or_rva, 16)
        sym = sess.session.findSymbolByRVA(func_rva, cvconst.SymTag.Function)
        if sym:
            return func_rva, int(sym.length), sym.name or ""
        return func_rva, 0, ""
    else:
        enum = sess.global_scope.findChildren(
            cvconst.SymTag.Function, name_or_rva, NSF_REGEX
        )
        if enum.count == 0:
            return 0, 0, ""
        sym = enum.Item(0)
        return int(sym.relativeVirtualAddress), int(sym.length), sym.name or name_or_rva


# ── Symbol info extraction ───────────────────────────────────────────────


def symbol_info(sym) -> dict[str, Any]:
    """Extract common fields from a DIA symbol."""
    info: dict[str, Any] = {}
    try:
        info["name"] = sym.name
    except Exception:
        pass
    try:
        info["symTag"] = sym_tag_name(int(sym.symTag))
    except Exception:
        pass
    try:
        info["rva"] = hex(int(sym.relativeVirtualAddress))
    except Exception:
        pass
    try:
        info["length"] = int(sym.length)
    except Exception:
        pass
    try:
        info["offset"] = int(sym.offset)
    except Exception:
        pass
    try:
        t = sym.type
        if t:
            info["type"] = type_name(t)
    except Exception:
        pass
    try:
        info["access"] = int(sym.access)
    except Exception:
        pass
    return info


def enum_children(parent, sym_tag, name=None, max_results=200) -> list[dict]:
    """Enumerate children of a DIA symbol by tag."""
    tag = getattr(cvconst.SymTag, sym_tag, None) if isinstance(sym_tag, str) else sym_tag
    if tag is None:
        raise ValueError(f"Unknown SymTag: {sym_tag}")
    if name is None or name == "*":
        enum = parent.findChildren(tag, None, 0)
    else:
        enum = parent.findChildren(tag, name, NSF_REGEX)
    results = []
    for i in range(min(enum.count, max_results)):
        results.append(symbol_info(enum.Item(i)))
    return results


# ── Prototype building ───────────────────────────────────────────────────


def build_prototype_from_sym(func) -> dict[str, Any]:
    """Build prototype dict from a DIA function symbol."""
    result: dict[str, Any] = {"name": func.name}

    ret_type = "void"
    cc_name = ""
    params: list[str] = []

    try:
        ft = func.type
        if ft:
            try:
                ret_type = type_name(ft.type)
            except Exception:
                pass
            try:
                cc_name = CC_NAMES.get(int(ft.callingConvention), "")
            except Exception:
                pass

            arg_types: list[str] = []
            args_enum = ft.findChildren(cvconst.SymTag.FunctionArgType, None, 0)
            for j in range(args_enum.count):
                arg_types.append(type_name(args_enum.Item(j).type))

            # Named params from function body
            data_children = func.findChildren(cvconst.SymTag.Data, None, 0)
            named: list[str] = []
            for j in range(min(data_children.count, 50)):
                child = data_children.Item(j)
                try:
                    cname = child.name
                    coffset = int(child.offset)
                    if cname and cname != "this" and coffset >= 4:
                        named.append(cname)
                except Exception:
                    pass

            for j, at in enumerate(arg_types):
                if at == "...":
                    params.append("...")
                else:
                    pname = named[j] if j < len(named) else f"a{j}"
                    params.append(f"{at} {pname}")
    except Exception:
        pass

    is_virtual = False
    try:
        is_virtual = bool(func.virtual)
    except Exception:
        pass

    prefix = "virtual " if is_virtual else ""
    cc_part = f" {cc_name}" if cc_name and cc_name != "__thiscall" else ""
    param_str = ", ".join(params)
    sig = f"{prefix}{ret_type}{cc_part} {func.name}({param_str})"
    sig = re.sub(r"\s+", " ", sig).strip()

    result["signature"] = sig
    result["return_type"] = ret_type
    result["calling_convention"] = cc_name
    result["params"] = params
    result["virtual"] = is_virtual
    return result


def get_prototype_by_rva(sess: Session, rva: int) -> dict:
    """Get prototype for the function at a specific RVA. Cached."""
    if rva in sess.proto_cache_by_rva:
        return sess.proto_cache_by_rva[rva]
    try:
        sym = sess.session.findSymbolByRVA(rva, cvconst.SymTag.Function)
        if sym and sym.name:
            result = build_prototype_from_sym(sym)
            result["rva"] = hex(rva)
            sess.proto_cache_by_rva[rva] = result
            return result
    except Exception:
        pass
    sess.proto_cache_by_rva[rva] = {}
    return {}


def get_prototype_by_name(sess: Session, func_name: str) -> dict:
    """Get prototype by name (first match). Cached."""
    if func_name in sess.proto_cache_by_name:
        return sess.proto_cache_by_name[func_name]
    try:
        enum = sess.global_scope.findChildren(cvconst.SymTag.Function, func_name, 0)
        if enum.count == 0:
            sess.proto_cache_by_name[func_name] = {}
            return {}
        func = enum.Item(0)
        result = build_prototype_from_sym(func)
        rva = int(func.relativeVirtualAddress)
        result["rva"] = hex(rva)
        sess.proto_cache_by_name[func_name] = result
        sess.proto_cache_by_rva[rva] = result
        return result
    except Exception:
        sess.proto_cache_by_name[func_name] = {}
        return {}


# ── Source file resolution ───────────────────────────────────────────────


def source_file_for_rva(sess: Session, rva: int, length: int = 1) -> str | None:
    """Get the source file path for a given RVA via line info."""
    try:
        lines = sess.session.findLinesByRVA(rva, length)
        if lines.count > 0:
            return lines.Item(0).sourceFile.fileName
    except Exception:
        pass
    return None


def source_file_for_type(sess: Session, tname: str) -> str | None:
    """Find the source file where a type's methods are defined."""
    try:
        enum = sess.global_scope.findChildren(cvconst.SymTag.UDT, tname, 0)
        if enum.count == 0:
            return None
        udt = enum.Item(0)
        methods = udt.findChildren(cvconst.SymTag.Function, None, 0)
        for i in range(min(methods.count, 50)):
            func = methods.Item(i)
            try:
                rva = int(func.relativeVirtualAddress)
                length = int(func.length)
                if rva > 0 and length > 0:
                    sf = source_file_for_rva(sess, rva, length)
                    if sf:
                        return sf
            except Exception:
                continue
    except Exception:
        pass
    return None


# ── Virtual method helpers ───────────────────────────────────────────────


def build_method_info(child) -> dict | None:
    """Extract method info dict from a DIA Function symbol."""
    try:
        mname = child.name
        ret_type = "void"
        params: list = []
        cc_name = ""

        try:
            ft = child.type
            if ft:
                try:
                    ret_type = type_name(ft.type)
                except Exception:
                    pass
                try:
                    cc_name = CC_NAMES.get(int(ft.callingConvention), "")
                except Exception:
                    pass
                args_enum = ft.findChildren(cvconst.SymTag.FunctionArgType, None, 0)
                for j in range(args_enum.count):
                    at = type_name(args_enum.Item(j).type)
                    params.append({"type": at, "name": f"a{j}"} if at != "..." else "...")
        except Exception:
            pass

        prefix = "virtual "
        cc_part = f" {cc_name}" if cc_name and cc_name != "__thiscall" else ""
        param_str = ", ".join(
            p if isinstance(p, str) else f"{p['type']} {p['name']}"
            for p in params
        )
        sig = f"{prefix}{ret_type}{cc_part} {mname}({param_str})"
        sig = re.sub(r"\s+", " ", sig).strip()

        return {
            "name": mname,
            "signature": sig,
            "return_type": ret_type,
            "params": params,
            "virtual": True,
        }
    except Exception:
        return None


def find_virtual_at_offset(udt, target: int) -> dict | None:
    """Search a UDT for a virtual method at the given vtable byte offset.
    Pass 1: match by virtualBaseOffset. Pass 2: sequential counting fallback."""
    target_idx = target // 4
    all_children = udt.findChildren(cvconst.SymTag.Null, None, 0)

    # Pass 1: match by virtualBaseOffset
    for i in range(all_children.count):
        child = all_children.Item(i)
        if int(child.symTag) != int(cvconst.SymTag.Function):
            continue
        try:
            if not bool(child.virtual):
                continue
        except Exception:
            continue
        try:
            vbase_offset = int(child.virtualBaseOffset)
            if vbase_offset == target:
                return build_method_info(child)
        except Exception:
            pass

    # Pass 2: sequential virtual method counting
    vidx = 0
    for i in range(all_children.count):
        child = all_children.Item(i)
        if int(child.symTag) != int(cvconst.SymTag.Function):
            continue
        try:
            if not bool(child.virtual):
                continue
        except Exception:
            continue
        if vidx == target_idx:
            return build_method_info(child)
        vidx += 1

    return None


def get_function_locals(sess: Session, func_rva: int) -> dict[int, str]:
    """Get local variables/params for a function. {stack_offset: 'name (type)'}."""
    result: dict[int, str] = {}
    try:
        sym = sess.session.findSymbolByRVA(func_rva, cvconst.SymTag.Function)
        if not sym:
            return result
        children = sym.findChildren(cvconst.SymTag.Data, None, 0)
        for i in range(min(children.count, 200)):
            child = children.Item(i)
            try:
                name = child.name
                offset = int(child.offset)
                tname = ""
                try:
                    t = child.type
                    if t and t.name:
                        tname = t.name
                except Exception:
                    pass
                if name:
                    label = name
                    if tname:
                        label += f" ({tname})"
                    result[offset] = label
            except Exception:
                pass
    except Exception:
        pass
    return result


def enum_functions(sess: Session) -> list[tuple[int, int, str]]:
    """Enumerate all functions in the PDB.

    Returns list of (rva, length, name) for every function with rva > 0.
    """
    results: list[tuple[int, int, str]] = []
    try:
        enum = sess.global_scope.findChildren(cvconst.SymTag.Function, None, 0)
        for i in range(enum.count):
            func = enum.Item(i)
            try:
                rva = int(func.relativeVirtualAddress)
                length = int(func.length)
                name = func.name or ""
                if rva > 0 and length > 0:
                    results.append((rva, length, name))
            except Exception:
                continue
    except Exception:
        pass
    return results


def parent_class(sess: Session, func_rva: int) -> str:
    """Get the parent class name for a method via PDB classParent.

    Returns empty string for free functions.
    """
    try:
        sym = sess.session.findSymbolByRVA(func_rva, cvconst.SymTag.Function)
        if sym and sym.classParent:
            return sym.classParent.name or ""
    except Exception:
        pass
    return ""
