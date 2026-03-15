/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _COKERNEL_H
#define _COKERNEL_H

#include <stdint.h>
#include <stddef.h>

/* Import shared layout constants */
#include "../include/shared.h"

/* Co-kernel entry point called from PMI handler */
void component_tick(void);

#endif /* _COKERNEL_H */
