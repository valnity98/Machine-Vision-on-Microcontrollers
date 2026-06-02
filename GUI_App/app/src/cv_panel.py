"""
cv_panel.py — REMOVED (legacy module).

All CV pipeline UI elements are defined statically in reference_window.ui
and driven directly by ReferenceWindowController in reference_window.py.

Firmware commands handled (camera_app.c handle_cv()):
  CV GET | RUN | EN | PRESET | BGCAP | BGSUB | BORDFILT
  CV THR | THRMODE | INV | FILTER | BLUR | MORPH | MORPHMODE
  CV CON | MINAREA | MAXAREA | ASPECT | CIRC | ROI

Do NOT import ClassicalVisionPanel from this file — it no longer exists.
"""
