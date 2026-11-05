"""Shared test settings.

Hypothesis draws new examples on every run, which finds more bugs locally. On CI (the CI variable is set) it uses a
fixed derandomized profile, so the same commit always runs the same examples and a red run can be repeated.
"""

import os

from hypothesis import settings

settings.register_profile("ci", derandomize=True, print_blob=True, deadline=None)
settings.load_profile("ci" if os.environ.get("CI") else "default")
