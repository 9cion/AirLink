"""Collect installed distribution notices for the bundled transport runtime."""
from importlib.metadata import distributions
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
out = root / "build" / "THIRD-PARTY-NOTICES.txt"
out.parent.mkdir(exist_ok=True)
parts = ["AirLink 0.1.0 — third-party notices\n",
         (root / "LICENSE").read_text(encoding="utf-8")]
for dist in sorted(distributions(), key=lambda d: d.metadata["Name"].lower()):
    parts.append(f"\n{'=' * 72}\n{dist.metadata['Name']} {dist.version}\n")
    for item in dist.files or []:
        if any(term in item.name.lower() for term in ("license", "copying", "notice")):
            path = Path(dist.locate_file(item))
            if path.is_file() and path.suffix.lower() not in (".py", ".pyc", ".pyd", ".dll"):
                try:
                    parts.append(f"\n{item}\n" + path.read_text(encoding="utf-8", errors="replace"))
                except OSError:
                    pass
python_license = Path(sys.base_prefix) / "LICENSE.txt"
if python_license.exists():
    parts.append("\nPython runtime\n" + python_license.read_text(encoding="utf-8"))
out.write_text("\n".join(parts), encoding="utf-8")
print(out)

