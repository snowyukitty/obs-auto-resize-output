// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <obs.h>

// Prefixed logging so plugin lines are easy to find in the OBS log.
#define ARO_LOG(level, format, ...) \
	blog(level, "[auto-resize-output] " format, ##__VA_ARGS__)
