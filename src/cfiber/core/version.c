/* SPDX-FileCopyrightText: 2026 Aoto
 * SPDX-License-Identifier: MIT */

#include "cfiber/version.h"

int cfiber_version(void) {
    return CFIBER_VERSION;
}

const char* cfiber_version_string(void) {
    return CFIBER_VERSION_STRING;
}
