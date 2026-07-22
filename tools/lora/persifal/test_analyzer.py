"""Prescan exe → SQLite hot cache with live progress."""

import sys, os, time

root = os.path.join(os.path.dirname(__file__), "..", "..", "..")
sys.path.insert(0, root)
os.chdir(root)

from provider.lora._state import SessionManager
from provider.lora.persifal._typedb import TypeDB
from provider.lora.persifal._typeprovider import TypeProvider, _IAT
from provider.lora.persifal._analyzer import Analyzer


def main():
    mgr = SessionManager()
    sess = mgr.load("target/hta.exe", "target/game.pdb")

    db = TypeDB("target/game.persifal.db")
    t_pre = time.time()
    counts = db.preheat()
    print(f"Preheated DB in {time.time() - t_pre:.1f}s: {counts}")

    provider = TypeProvider(db, image_base=sess.pe.OPTIONAL_HEADER.ImageBase)
    provider.annotate_batch(_IAT)
    t_pre = time.time()
    n_types = provider.preheat()
    print(f"Preheated {n_types} types in {time.time() - t_pre:.1f}s")

    az = Analyzer(sess, provider=provider)

    print(f"Cache: {az.cache.db_file}")
    cached = az.cache.count()
    print(f"Already cached: {cached}")

    # enumerate functions from TypeDB (no PDB COM needed)
    all_funcs = db.all_functions()
    total = len(all_funcs)
    print(f"{total} functions from TypeDB")

    cached_rvas = az.cache.all_rvas()
    todo = [(rva, length, name) for rva, length, name in all_funcs if rva not in cached_rvas]
    print(f"To process: {len(todo)}  (skipping {total - len(todo)} cached)")

    if not todo:
        print("Nothing to do.")
        _print_stats(az, db)
        return

    t0 = time.time()
    done = 0
    errors = 0
    last_name = ""
    err_counts = {}  # msg → count

    for i, (rva, length, name) in enumerate(todo):
        try:
            az.process(rva, length, name=name)
            done += 1
            last_name = name
        except Exception as e:
            errors += 1
            last_name = f"ERR {name}: {e}"
            key = f"{type(e).__name__}: {str(e)[:80]}"
            err_counts[key] = err_counts.get(key, 0) + 1

        # commit every 200
        if (i + 1) % 200 == 0:
            az.cache.commit()

        # live progress every function
        elapsed = time.time() - t0
        rate = (i + 1) / elapsed if elapsed > 0 else 0
        eta = (len(todo) - i - 1) / rate if rate > 0 else 0
        pct = (i + 1) / len(todo) * 100

        # truncate name for display
        disp = last_name[:50].ljust(50)
        sys.stdout.write(
            f"\r  [{i+1}/{len(todo)}] {pct:5.1f}%  "
            f"{rate:5.0f} f/s  ETA {eta:5.0f}s  "
            f"ok={done} err={errors}  {disp}"
        )
        sys.stdout.flush()

    az.cache.commit()
    n_new = provider.flush_new_types()
    db.commit()
    print(f"\n  flushed {n_new} new runtime types")
    elapsed = time.time() - t0

    print(f"\n\nDone: {done} new, {errors} errors in {elapsed:.1f}s")

    if err_counts:
        print(f"\nTop errors ({len(err_counts)} types):")
        for msg, cnt in sorted(err_counts.items(), key=lambda x: -x[1])[:15]:
            print(f"  {cnt:5d}x {msg}")

    _print_stats(az, db)


def _print_stats(az: Analyzer, db: TypeDB):
    st = az.stats()
    print(f"\n  in_db:     {st['in_db']}")
    print(f"  in_memory: {st['in_memory']}")

    conn = az.cache._open()
    row = conn.execute(
        "SELECT COUNT(*), SUM(length), AVG(length), MAX(length) FROM funcs"
    ).fetchone()
    if row[0]:
        print(f"\n  total funcs: {row[0]}")
        print(f"  total bytes: {row[1]:,}")
        print(f"  avg size:    {row[2]:.0f}")
        print(f"  max size:    {row[3]:,}")

    row = conn.execute(
        "SELECT AVG(edge_count), MAX(edge_count), "
        "SUM(CASE WHEN loop_count > 0 THEN 1 ELSE 0 END) FROM funcs"
    ).fetchone()
    if row[0] is not None:
        print(f"  avg edges:   {row[0]:.1f}")
        print(f"  max edges:   {row[1]}")
        print(f"  with loops:  {row[2]}")

    # Runtime types created
    rt = db.conn.execute(
        "SELECT COUNT(*) FROM types WHERE name LIKE '%_vft' OR name LIKE '%*' OR name LIKE '%&'"
    ).fetchone()
    print(f"  runtime types added: {rt[0]}")


if __name__ == "__main__":
    main()
