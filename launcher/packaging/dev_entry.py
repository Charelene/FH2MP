"""Packaged developer launcher: always use the separate dev profile."""
import sys

from fh2mp_launcher.app import main_dev

sys.exit(main_dev())
