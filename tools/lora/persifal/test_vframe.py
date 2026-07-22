"""Debug: check vframe offset at specific RVA."""

import sys, os

root = os.path.join(os.path.dirname(__file__), "..", "..", "..")
sys.path.insert(0, root)
os.chdir(root)

from provider.lora._state import SessionManager
from provider.lora._pdb import resolve_function
from provider.lora.persifal._decode import decode
from provider.lora.persifal._enrich import enrich, build_known
from provider.lora.persifal._ir import *
from provider.lora.persifal._vm import Machine


def main():
    mgr = SessionManager()
    sess = mgr.load("target/hta.exe", "target/game.pdb")
    func_rva, func_len, func_name = resolve_function(sess, "m3d::Application::init")

    raw = decode(sess, func_rva, func_len)
    known = build_known(sess, func_rva)

    # Debug build_known manually
    from provider.lora.persifal._vm import Stack as VMStack, Symbol
    known2 = VMStack()
    from pydia2 import cvconst
    sym = sess.session.findSymbolByRVA(func_rva, cvconst.SymTag.Function)
    data = sym.findChildren(cvconst.SymTag.Data, None, 0)
    for i in range(data.count):
        child = data.Item(i)
        try:
            loc = int(child.locationType)
            if loc == 2:
                name = child.name or ""
                typ = pdb_mod.type_name(child.type) if child.type else ""
                off = int(child.offset)
                size = int(child.type.length) if child.type else 4
                if name:
                    print(f"  Trying: vf[{off:+d}] = {typ} {name} ({size}B)")
                    try:
                        known2[off] = Symbol(name=f"{typ} {name}", size=max(size, 1))
                        print(f"    OK")
                    except KeyError as e:
                        print(f"    OVERLAP: {e}")
        except Exception as e:
            print(f"  ERROR on child {i}: {e}")

    print(f"\nPDB known entries: {sum(1 for _ in known)}")
    for ref in known:
        print(f"  vf[{ref.base:+5d}]  {ref.type.name} ({ref.type.size}B)")

    # Debug: raw PDB locals
    from pydia2 import cvconst
    from provider.lora import _pdb as pdb_mod
    from provider.lora._constants import LOC_NAMES
    sym = sess.session.findSymbolByRVA(func_rva, cvconst.SymTag.Function)
    data = sym.findChildren(cvconst.SymTag.Data, None, 0)
    print(f"\nRaw PDB data children: {data.count}")
    for i in range(data.count):
        child = data.Item(i)
        name = child.name or ""
        loc = int(child.locationType)
        try:
            off = int(child.offset)
        except:
            off = -999
        typ = pdb_mod.type_name(child.type) if child.type else "?"
        print(f"  {name:20s} loc={LOC_NAMES.get(loc, str(loc)):8s} off={off:+6d} type={typ}")

    print()

    # Simulate esp_delta up to 0x1a8d39
    machine = Machine()
    raw.reset()
    for node in raw:
        if node.rva >= 0x1a8d39:
            print(f"At {hex(node.rva)}: esp_delta = {machine.esp_delta}")
            print(f"  [esp+0x110] → vf[{machine.vframe(0x110)}]")
            print(f"  Expected: cmdLine at vf[+20]")
            break
        machine.eat(node)


if __name__ == "__main__":
    main()
