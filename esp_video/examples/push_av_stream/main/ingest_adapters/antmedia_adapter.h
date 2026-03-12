/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file antmedia_adapter.h
 * @brief Ant Media Server CMAF/DASH ingest adapter
 */

#pragma once

#include "ingest_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get Ant Media Server adapter operations
 * @return Pointer to operations structure
 */
const ingest_transport_ops_t *antmedia_adapter_get_ops(void);

#ifdef __cplusplus
}
#endif
