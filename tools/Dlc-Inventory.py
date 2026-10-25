"""Inspect extracted FM4 packages without changing their files or licenses."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sqlite3
import zipfile
from datetime import datetime, timezone


PACKAGE_NAME = re.compile(r"(?:[0-9a-f]{16}|[0-9a-f]{42})", re.I)
TITLES = {
    1: "Disc 2: 1950-2000", 2: "Disc 2: 2001-2005",
    3: "Disc 2: 2006-2011", 4: "Disc 2: Autovista",
    21: "Bonus Track Pack: Benchmark High Speed Ring and Top Gear Soccer",
    23: "Launch, preorder, American Muscle, BMW and VIP/LCE bundle",
    25: "November Speed", 26: "December IGN", 27: "January Jalopnik",
    28: "Hyundai Veloster Bonus", 29: "February ALMS", 30: "March Pirelli",
    31: "April Alpinestars", 32: "May TopGear", 34: "Porsche Expansion",
    35: "June Meguiar's", 36: "SRT Viper Bonus and Challenger content",
    37: "July Car Pack", 38: "August Playseat", 39: "September Pennzoil",
}


def packages(root):
    """Accept the package root or the launcher's one-level container layout."""
    candidates = list(root.iterdir())
    nested = root / "00000002"
    if nested.is_dir():
        candidates += list(nested.iterdir())
    result = {}
    for path in candidates:
        if path.is_dir() and PACKAGE_NAME.fullmatch(path.name) and (path / "Media").is_dir():
            key = path.name.upper()
            if key in result and result[key].resolve() != path.resolve():
                raise ValueError(f"Duplicate package identity: {path.name}")
            result[key] = path
    return [result[k] for k in sorted(result)]


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def inspect_package(path):
    files = sorted((p for p in path.rglob("*") if p.is_file()), key=lambda p: str(p).lower())
    markers = list((path / "Media").glob("*.puboffer"))
    publisher_ids = sorted({int(p.stem) for p in markers if p.stem.isdecimal()})
    record = {"package_directory_id": path.name, "path": str(path),
              "title": "; ".join(TITLES.get(n, f"Publisher offer {n}") for n in publisher_ids),
              "publisher_offering_ids": publisher_ids,
              "content_type": "00000002 (marketplace; directory convention, not an STFS header)",
              "marketplace_offer_type": None,
              "original_stfs_content_id": None,
              "granted_license_mask": None,
              "entitlement_status": "Unknown: extracted assets do not establish original licenses",
              "file_count": len(files), "total_bytes": sum(p.stat().st_size for p in files),
              "metadata": [], "databases": [], "offers": [], "content_mappings": [], "cars": [], "events": [],
              "asset_car_names": [], "archives": [], "expansion_skus": [], "tracks": []}
    car_names = set()
    for file in files:
        relative = file.relative_to(path).as_posix()
        parts = relative.split("/")
        lower = [part.lower() for part in parts]
        if file.suffix.lower() == ".carbin" and "cars" in lower:
            pos = lower.index("cars") + 1
            if pos < len(parts) - 1:
                car_names.add(parts[pos])
        if file.suffix.lower() in (".puboffer", ".sku", ".slt", ".header") or file.name.lower() in ("licensemasks", "zipmount.xml"):
            record["metadata"].append({"path": relative, "bytes": file.stat().st_size, "sha256": digest(file)})
        if file.suffix.lower() == ".sku":
            record["expansion_skus"].append(file.read_text(errors="replace").strip())
        if file.suffix.lower() == ".zip":
            archive = {"path": relative, "bytes": file.stat().st_size}
            try:
                with zipfile.ZipFile(file) as container:
                    names = container.namelist()
                    archive["member_count"] = len(names)
                    for name in names:
                        member_parts = name.replace("\\", "/").split("/")
                        member_lower = [v.lower() for v in member_parts]
                        if name.lower().endswith(".carbin") and "cars" in member_lower:
                            pos = member_lower.index("cars") + 1
                            if pos < len(member_parts) - 1:
                                car_names.add(member_parts[pos])
            except zipfile.BadZipFile:
                archive["note"] = "Nonstandard archive; member list not decoded"
            record["archives"].append(archive)
        if file.suffix.lower() != ".slt":
            continue
        # SQLite URI read-only mode prevents creation, journals and source edits.
        with sqlite3.connect(file.resolve().as_uri() + "?mode=ro", uri=True) as db:
            db.row_factory = sqlite3.Row
            db.execute("PRAGMA query_only=ON")
            tables = {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            record["databases"].append({"path": relative, "sha256": digest(file),
                                         "tables": {t: db.execute('SELECT count(*) FROM "' + t.replace('"', '""') + '"').fetchone()[0] for t in sorted(tables)}})
            for table, destination, columns in [
                ("ContentOffers", "offers", None),
                ("ContentOffersMapping", "content_mappings", None),
                ("Data_Car", "cars", "Id,Year,MediaName,ContentId,IsInstalled,IsPurchased,HasCarExperience,IsCarExperienceUnlocked"),
                ("Events", "events", "Id,Name,IsDLC,CareerTypeId"),
            ]:
                if table in tables:
                    rows = [dict(r) for r in db.execute(f'SELECT {columns or "*"} FROM "{table}"')]
                    for row in rows:
                        if "LicenseMask" in row:
                            row["required_license_mask_hex"] = f'{row["LicenseMask"] & 0xFFFFFFFF:08X}'
                        if "OfferId" in row:
                            row["offer_id_hex"] = f'{row["OfferId"] & 0xFFFFFFFFFFFFFFFF:016X}'
                    record[destination] += rows
            for table in sorted(tables):
                if "track" in table.lower() and table not in ("CarTrackOffsets",):
                    record["tracks"].append({"table": table, "rows": [dict(r) for r in db.execute('SELECT * FROM "' + table.replace('"', '""') + '"')]})
    record["asset_car_names"] = sorted(car_names)
    # A zero-sized license marker is not a grant of every database requirement.
    record["blank_puboffer_files"] = all(not p.read_bytes().strip() for p in markers)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows = [inspect_package(p) for p in packages(args.root)]
    result = {"schema": 1, "captured_utc": datetime.now(timezone.utc).isoformat(),
              "root": str(args.root.resolve()), "package_count": len(rows),
              "note": "Required database masks are not granted entitlements. Package directory IDs are not authenticated STFS content IDs. No source files were modified.",
              "packages": rows}
    # Exclusive creation keeps previous inventory evidence intact.
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2, ensure_ascii=False)
        output.write("\n")
    print(f"Inventoried {len(rows)} packages into {args.output}")


if __name__ == "__main__":
    main()
