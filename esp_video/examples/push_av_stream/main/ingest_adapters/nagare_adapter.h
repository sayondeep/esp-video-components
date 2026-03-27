/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

/**
 * @file nagare_adapter.h
 * @brief nagare-media/ingest adapter (DASH-IF Interface-2 over plain HTTP)
 *
 * https://github.com/nagare-media/ingest
 */

#pragma once

#include "ingest_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

const ingest_transport_ops_t *nagare_adapter_get_ops(void);

#ifdef __cplusplus
}
#endif
