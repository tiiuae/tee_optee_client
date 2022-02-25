/*
 * Copyright 2022, Unikie
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef _SEL4_SERIALIZER_H_
#define _SEL4_SERIALIZER_H_

#include <stdio.h>
#include "tee_client_api.h"
#include "sel4_req.h"

/* Fixed "random" constants for dev purposes */
#define CTX_TA_FD          5
#define TA_SESSION_ID   0x81

TEEC_Result sel4_serialize_params(TEEC_Operation *operation, struct serialized_param **param_buf, uint32_t *param_buf_len);
TEEC_Result sel4_deserialize_params(TEEC_Operation *operation, struct serialized_param *param_buf, uint32_t param_buf_len);

#endif  /* _SEL4_SERIALIZER_H_ */