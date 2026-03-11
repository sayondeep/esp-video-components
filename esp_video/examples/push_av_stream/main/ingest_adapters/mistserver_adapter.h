/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file mistserver_adapter.h
 * @brief MistServer adapter
 */

#pragma once

#include "ingest_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

const ingest_transport_ops_t *mistserver_adapter_get_ops(void);

#ifdef __cplusplus
}
#endif
