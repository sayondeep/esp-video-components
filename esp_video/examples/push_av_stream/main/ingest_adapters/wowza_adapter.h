/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file wowza_adapter.h
 * @brief Wowza Streaming Engine CMAF/DASH ingest adapter
 */

#pragma once

#include "ingest_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get Wowza adapter operations
 * @return Pointer to operations structure
 */
const ingest_transport_ops_t *wowza_adapter_get_ops(void);

#ifdef __cplusplus
}
#endif
