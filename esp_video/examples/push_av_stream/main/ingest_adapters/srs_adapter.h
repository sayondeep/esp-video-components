/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file srs_adapter.h
 * @brief SRS (Simple Realtime Server) adapter
 */

#pragma once

#include "ingest_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

const ingest_transport_ops_t *srs_adapter_get_ops(void);

#ifdef __cplusplus
}
#endif
