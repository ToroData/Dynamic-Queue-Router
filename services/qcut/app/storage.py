import os
from dataclasses import dataclass

@dataclass(frozen=True)
class Storage:
    base_dir: str

    def abs(self, relpath: str) -> str:
        relpath = relpath.lstrip("/").replace("..", "")
        return os.path.join(self.base_dir, relpath)

def ensure_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)
