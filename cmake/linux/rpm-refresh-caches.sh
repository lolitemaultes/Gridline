#!/bin/sh
# Gridline .rpm, after install and after removal: refreshes the desktop's file type and
# application caches, so Gridline's project types and its desktop entry (Open with) take effect.
# Distributions whose shared-mime-info and desktop-file-utils have file triggers do this
# themselves; elsewhere this does it. Never fails the transaction.
if command -v update-mime-database > /dev/null 2>&1; then update-mime-database /usr/share/mime > /dev/null 2>&1 || :; fi
if command -v update-desktop-database > /dev/null 2>&1; then update-desktop-database -q /usr/share/applications > /dev/null 2>&1 || :; fi
exit 0
