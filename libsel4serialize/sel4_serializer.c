/*
 * Copyright 2022, Unikie
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <tee_client_api_extensions.h>
#include <tee_client_api.h>
#include <teec_trace.h>
#include <unistd.h>

#include <linux/tee.h>

#include "sel4_serializer.h"

/* From OPTEE OS */
#define TEE_PARAM_TYPE_NONE             0
#define TEE_PARAM_TYPE_VALUE_INPUT      1
#define TEE_PARAM_TYPE_VALUE_OUTPUT     2
#define TEE_PARAM_TYPE_VALUE_INOUT      3
#define TEE_PARAM_TYPE_MEMREF_INPUT     5
#define TEE_PARAM_TYPE_MEMREF_OUTPUT    6
#define TEE_PARAM_TYPE_MEMREF_INOUT     7

static TEEC_Result allocate_serialize_buf(TEEC_Operation *operation,
                                          void **param,
                                          uint32_t *param_len)
{
    size_t len = sizeof(struct serialized_param) * TEEC_CONFIG_PAYLOAD_REF_COUNT;
    void *buf = NULL;
    uint32_t param_type = 0;

    for (int i = 0; i < TEEC_CONFIG_PAYLOAD_REF_COUNT; i++) {
        param_type = TEEC_PARAM_TYPE_GET(operation->paramTypes, i);

        switch (param_type) {
        case TEEC_NONE:
            break;
        case TEEC_VALUE_INPUT:
        case TEEC_VALUE_OUTPUT:
        case TEEC_VALUE_INOUT:
            DBG_ABORT();
            break;
        case TEEC_MEMREF_TEMP_INPUT:
        case TEEC_MEMREF_TEMP_OUTPUT:
        case TEEC_MEMREF_TEMP_INOUT:
            len += operation->params[i].tmpref.size;
            break;
        case TEEC_MEMREF_WHOLE:
            len += operation->params[i].memref.parent->size;
            break;
        case TEEC_MEMREF_PARTIAL_INPUT:
        case TEEC_MEMREF_PARTIAL_OUTPUT:
        case TEEC_MEMREF_PARTIAL_INOUT:
            DBG_ABORT();
            break;
        default:
            EMSG("Unknown parameter");
            return TEEC_ERROR_BAD_PARAMETERS;
        }
    }

    buf = malloc(len);
    if (!buf) {
        EMSG("out of memory");
        return TEEC_ERROR_OUT_OF_MEMORY;
    }

    *param = buf;
    *param_len = len;

    IMSG("param_len: %ld", len);

    memset(buf, 0x0, len);

    return TEEC_SUCCESS;
}

static void serialize_tmpref(TEEC_TempMemoryReference *tmpref,
                            uint32_t param_type,
                            struct serialized_param *param)
{
    switch (param_type) {
    case TEEC_MEMREF_TEMP_INPUT:
        param->param_type = TEE_PARAM_TYPE_MEMREF_INPUT;
        break;
    case TEEC_MEMREF_TEMP_OUTPUT:
        param->param_type = TEE_PARAM_TYPE_MEMREF_OUTPUT;
        break;
    /* TEEC_MEMREF_TEMP_INOUT
     * no other values passed by caller
     */
    default:
        param->param_type = TEE_PARAM_TYPE_MEMREF_INOUT;
        break;
    }

    param->val_len = tmpref->size;
    memcpy(param->value, tmpref->buffer, param->val_len);

    IMSG("TEEC_MEMREF_TEMP [%d] len: %d", param_type, param->val_len);

    hexdump(param->value, param->val_len);
}

static TEEC_Result serialize_memref_whole(
                                TEEC_RegisteredMemoryReference *memref,
                                struct serialized_param *param)
{
    const uint32_t inout = TEEC_MEM_INPUT | TEEC_MEM_OUTPUT;
    uint32_t flags = memref->parent->flags & inout;
    uint32_t param_type = 0;

    if (flags & (TEEC_MEM_INPUT | TEEC_MEM_OUTPUT))
        param_type = TEEC_MEMREF_TEMP_INOUT;
    else if (flags & TEEC_MEM_INPUT)
        param_type = TEEC_MEMREF_TEMP_INPUT;
    else if (flags & TEEC_MEM_OUTPUT)
        param_type = TEEC_MEMREF_TEMP_OUTPUT;
    else {
        EMSG("Unknown flags: 0x%x", flags);
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    param->val_len = memref->parent->size;
    memcpy(param->value, memref->parent->buffer, param->val_len);

    IMSG("TEEC_MEMREF_WHOLE [%d] len: %d", param_type, param->val_len);
    hexdump(param->value, param->val_len);

    return TEEC_SUCCESS;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
TEEC_Result sel4_serialize_params(TEEC_Operation *operation,
                                  struct serialized_param **param_buf,
                                  uint32_t *param_buf_len)
{
    TEEC_Result err = TEEC_ERROR_GENERIC;
    TEEC_Operation *op = operation;
    uint32_t param_type = 0;
    void *buf = NULL;
    uint32_t len = 0;
    struct serialized_param *param = NULL;

    if (!param_buf || !param_buf_len) {
        IMSG("Invalid param");
        err = TEEC_ERROR_BAD_PARAMETERS;
        goto out;
    }

    /* Create empty operation with NONE params to be sent to TEE*/
    if (!operation) {
        IMSG("No params");
        op = malloc(sizeof(TEEC_Operation));
        if (!op) {
            EMSG("out of memory");
            err = TEEC_ERROR_OUT_OF_MEMORY;
            goto out;
        }
        /* paramTypes: TEEC_NONE */
        memset(op, 0x0, sizeof(TEEC_Operation));
    }

    err = allocate_serialize_buf(op, &buf, &len);
    if (err != TEEC_SUCCESS) {
        goto out;
    }

    param = (struct serialized_param *)buf;

    /* Buffer size is calculated from param->val_len in allocate_serialize_buf()
     * No need to check buffer end during the loop.
     */
    for (int i = 0; i < TEEC_CONFIG_PAYLOAD_REF_COUNT; i++) {
        param_type = TEEC_PARAM_TYPE_GET(op->paramTypes, i);

        switch (param_type) {
        case TEEC_NONE:
            IMSG("TEEC_NONE");
            param->param_type = TEE_PARAM_TYPE_NONE;
            param->val_len = 0;
            break;
        case TEEC_VALUE_INPUT:
        case TEEC_VALUE_OUTPUT:
        case TEEC_VALUE_INOUT:
            DBG_ABORT();
            break;
        case TEEC_MEMREF_TEMP_INPUT:
        case TEEC_MEMREF_TEMP_OUTPUT:
        case TEEC_MEMREF_TEMP_INOUT:
            serialize_tmpref(&op->params[i].tmpref, param_type, param);
            break;
        case TEEC_MEMREF_WHOLE:
            err = serialize_memref_whole(&op->params[i].memref, param);
            if (err) {
                goto out;
            }
            break;
        case TEEC_MEMREF_PARTIAL_INPUT:
        case TEEC_MEMREF_PARTIAL_OUTPUT:
        case TEEC_MEMREF_PARTIAL_INOUT:
            DBG_ABORT();
            break;
        default:
            err = TEEC_ERROR_BAD_PARAMETERS;
            goto out;
        }

        /* Move to next parameter */
        param = (struct serialized_param *)(param->value + param->val_len);
    };

    *param_buf = (struct serialized_param *)buf;
    *param_buf_len = len;

    err = TEEC_SUCCESS;

out:
    if (err != TEEC_SUCCESS) {
        free(buf);
    }

    /* free temporary operation allocation */
    if (!operation) {
        free(op);
    }

    return err;
}
#pragma GCC diagnostic pop

static TEEC_Result deserialize_tmpref(uint32_t param_type,
                                      TEEC_Parameter *teec_param,
                                      struct serialized_param *param
)
{
    if (param_type != param->param_type) {
        EMSG("Invalid param type: %d / %d", param_type, param->param_type);
        return TEEC_ERROR_BAD_FORMAT;
    }

    if (param->val_len > teec_param->tmpref.size) {
        EMSG("Invalid param size: %d / %ld", param->val_len, teec_param->tmpref.size);
        return TEEC_ERROR_BAD_FORMAT;
    }

    teec_param->tmpref.size = param->val_len;
    memcpy(teec_param->tmpref.buffer, param->value, param->val_len);

    IMSG("TEEC_MEMREF_TEMP [%d] len: %d", param_type, param->val_len);
    hexdump(param->value, param->val_len);

    return TEEC_SUCCESS;
}

static TEEC_Result deserialize_memref(uint32_t param_type,
                                      TEEC_Parameter *teec_param,
                                      struct serialized_param *param
)
{
    /* process only output params */
    if (!(teec_param->memref.parent->flags & TEEC_MEM_OUTPUT)) {
        IMSG("TEEC_MEMREF_WHOLE INPUT (NOP)");
        return TEEC_SUCCESS;
    }

    if (param->param_type != TEE_PARAM_TYPE_MEMREF_OUTPUT &&
        param->param_type != TEE_PARAM_TYPE_MEMREF_INOUT) {
        EMSG("Invalid msg type: %d", param->param_type);
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    if (param->val_len > teec_param->memref.parent->size) {
        EMSG("Buffer size mismatch: %d / %ld", param->val_len,
            teec_param->memref.parent->size);
        return TEEC_ERROR_BAD_FORMAT;
    }

    teec_param->memref.parent->size = param->val_len;
    memcpy(teec_param->memref.parent->buffer,
            param->value, param->val_len);

    IMSG("TEEC_MEMREF_WHOLE [%d] len: %d", param_type, param->val_len);
    hexdump(param->value, param->val_len);

    return TEEC_SUCCESS;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
TEEC_Result sel4_deserialize_params(TEEC_Operation *operation,
                                    struct serialized_param *param_buf,
                                    uint32_t param_buf_len)
{
    TEEC_Result err = TEEC_ERROR_GENERIC;
    struct serialized_param *param = param_buf;
    uintptr_t buf_end = (uintptr_t)param_buf + param_buf_len;
    uint32_t param_type = 0;

    if (!operation) {
        IMSG("No params");
        return TEEC_SUCCESS;
    }

    if (!param_buf) {
        IMSG("Invalid params");
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    for (int i = 0; i < TEEC_CONFIG_PAYLOAD_REF_COUNT; i++) {
        if ((uintptr_t)param >= buf_end) {
            EMSG("Buffer overflow");
            return TEEC_ERROR_EXCESS_DATA;
        }

        param_type = TEEC_PARAM_TYPE_GET(operation->paramTypes, i);

        switch (param_type) {
        case TEEC_NONE:
            IMSG("TEEC_NONE");
            break;
        case TEEC_VALUE_INPUT:
        case TEEC_VALUE_OUTPUT:
        case TEEC_VALUE_INOUT:
            DBG_ABORT();
            break;
        case TEEC_MEMREF_TEMP_INPUT:
            break;
        case TEEC_MEMREF_TEMP_OUTPUT:
        case TEEC_MEMREF_TEMP_INOUT:
            err = deserialize_tmpref(param_type, &operation->params[i], param);
            if (err) {
                return err;
            }
            break;
        case TEEC_MEMREF_WHOLE:
            err = deserialize_memref(param_type, &operation->params[i], param);
            if (err) {
                return err;
            }
            break;
        case TEEC_MEMREF_PARTIAL_INPUT:
        case TEEC_MEMREF_PARTIAL_OUTPUT:
        case TEEC_MEMREF_PARTIAL_INOUT:
            DBG_ABORT();
            break;
        default:
            return TEEC_ERROR_BAD_PARAMETERS;
        }

        /* Move to next parameter */
        param = (struct serialized_param *)(param->value + param->val_len);
    };

    return TEEC_SUCCESS;
}
#pragma GCC diagnostic pop
