"""Debug: check PDB params and locals for Application::init."""

import sys, os

root = os.path.join(os.path.dirname(__file__), "..", "..", "..")
sys.path.insert(0, root)
os.chdir(root)

from provider.lora._state import SessionManager
from provider.lora._pdb import resolve_function
from provider.lora import _pdb as pdb_mod


def main():
    mgr = SessionManager()
    sess = mgr.load("target/hta.exe", "target/game.pdb")
    func_rva, func_len, func_name = resolve_function(sess, "m3d::Application::init")
    print(f"{func_name} at {hex(func_rva)}, {func_len} bytes\n")

    from pydia2 import cvconst
    from provider.lora._constants import LOC_NAMES, REG_NAMES_X86

    sym = sess.session.findSymbolByRVA(func_rva, cvconst.SymTag.Function)
    if not sym:
        print("No symbol!")
        return

    data = sym.findChildren(cvconst.SymTag.Data, None, 0)
    print(f"Locals/params: {data.count}\n")
    print(f"{'name':20s}  {'type':30s}  {'loc':10s}  {'offset':>8s}  {'reg':8s}")
    print(f"{'---':20s}  {'---':30s}  {'---':10s}  {'---':>8s}  {'---':8s}")

    for i in range(min(data.count, 100)):
        child = data.Item(i)
        name = child.name or ""
        try:
            typ = pdb_mod.type_name(child.type) if child.type else "?"
        except:
            typ = "?"
        try:
            loc = int(child.locationType)
            loc_name = LOC_NAMES.get(loc, str(loc))
        except:
            loc_name = "?"
        try:
            off = int(child.offset)
        except:
            off = 0
        try:
            reg = int(child.registerId)
            reg_name = REG_NAMES_X86.get(reg, f"reg({reg})")
        except:
            reg_name = ""

        print(f"  {name:20s}  {typ:30s}  {loc_name:10s}  {off:+8d}  {reg_name:8s}")


if __name__ == "__main__":
    main()
