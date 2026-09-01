/*
 * Copyright (C) 2026 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "nautilus-list-base.h"

G_BEGIN_DECLS

#define NAUTILUS_TYPE_COLUMNS_VIEW (nautilus_columns_view_get_type())

G_DECLARE_FINAL_TYPE (NautilusColumnsView, nautilus_columns_view, NAUTILUS, COLUMNS_VIEW, NautilusListBase)

NautilusColumnsView *nautilus_columns_view_new (void);

G_END_DECLS
