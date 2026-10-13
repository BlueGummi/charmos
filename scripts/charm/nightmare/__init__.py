"""The nightmare config pipeline

suite TOML  ──►  suite.load()   the model, defaults, validation
                 codec.render() one boot's command line
                 codec.write()  the n.txt artifact
                      │
                      ▼   CMDLINE=n.txt
            scripts/mkiso.py  ──► limine.conf ──► iso ──► boot
"""

from .codec import BootRequest, CodecError, build_args, build_command, render, write
from .suite import Diagnostic, Suite, SuiteError, load

__all__ = [
    "BootRequest",
    "CodecError",
    "Diagnostic",
    "Suite",
    "SuiteError",
    "build_args",
    "build_command",
    "load",
    "render",
    "write",
]
