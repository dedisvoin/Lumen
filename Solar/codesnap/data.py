"""
CodeSnap data structures
"""
from dataclasses import dataclass, field
from typing import Optional, List


@dataclass
class CodeSnapData:
    text: str
    file_path: Optional[str] = None
    language_id: Optional[str] = None
    line_from: int = 0
    line_to: int = 0
    start_col: int = 0
    end_col: int = 0
    styles: Optional[List[int]] = None
    lexer_name: Optional[str] = None
    start_pos: int = -1
