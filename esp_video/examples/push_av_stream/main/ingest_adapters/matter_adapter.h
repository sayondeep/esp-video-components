/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file matter_adapter.h
 * @brief Matter push_av_server adapter implementation
 *
 * This adapter implements the ingest_transport interface for Matter's
 * push_av_server.
 */

#pragma once

#include "ingest_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the Matter adapter operations table
 *
 * @return Pointer to the adapter operations (static, never NULL)
 */
const ingest_transport_ops_t *matter_adapter_get_ops(void);

#ifdef __cplusplus
}
#endif
