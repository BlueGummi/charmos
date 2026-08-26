"""The nightmare config pipeline (DESIGN pole P2).

    suite TOML  ──►  suite.load()   the model, defaults, validation
                     codec.render() one boot's command line
                     codec.write()  the n.txt artifact
                          │
                          ▼   CMDLINE=n.txt
                cmake/gen_limine_conf.cmake ──► limine.conf ──► iso ──► boot

Nothing here knows what a campaign is; it renders one boot at a time from a
validated task, and the runner supplies the identity that varies.
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
