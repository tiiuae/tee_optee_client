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

#ifdef SEL4_PRINT_PARAM_MEMREF
#define HEXDUMP(...) dump_buffer(__VA_ARGS__)
#else /* SEL4_PRINT_PARAM_MEMREF */
#define HEXDUMP(...)
#endif /* SEL4_PRINT_PARAM_MEMREF */

#define MIN(x, y) (((x) < (y)) ? (x) : (y))

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
            len += sizeof(TEEC_Value);
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

    buf = calloc(1, len);
    if (!buf) {
        EMSG("out of memory");
        return TEEC_ERROR_OUT_OF_MEMORY;
    }

    *param = buf;
    *param_len = len;

    return TEEC_SUCCESS;
}

static void serialize_tmpref(TEEC_TempMemoryReference *tmpref,
                            struct serialized_param *param)
{
    /* param_type is already set by caller */

    param->val_len = tmpref->size;

    IMSG("TEEC_MEMREF_TEMP [%d] len: %d", param->param_type, param->val_len);

    if (!tmpref->buffer) {
        IMSG("no buffer");
        return;
    }

    memcpy(param->value, tmpref->buffer, param->val_len);

    HEXDUMP("", param->value, param->val_len);
}

static TEEC_Result
serialize_memref_whole(TEEC_RegisteredMemoryReference *memref,
                       struct serialized_param *param)
{
    const uint32_t inout = TEEC_MEM_INPUT | TEEC_MEM_OUTPUT;
    uint32_t flags = memref->parent->flags & inout;

    /* replace TEEC_MEMREF_WHOLE with proper TEEC_MEMREF_TEMP */
    if (flags == inout)
        param->param_type = TEEC_MEMREF_TEMP_INOUT;
    else if (flags & TEEC_MEM_INPUT)
        param->param_type = TEEC_MEMREF_TEMP_INPUT;
    else if (flags & TEEC_MEM_OUTPUT)
        param->param_type = TEEC_MEMREF_TEMP_OUTPUT;
    else {
        EMSG("Unknown flags: 0x%x", flags);
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    if (!memref->parent) {
        EMSG("invalid parent");
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    param->val_len = memref->parent->size;

    IMSG("TEEC_MEMREF_WHOLE [%d] len: %d, f: 0x%x", param->param_type,
        param->val_len, memref->parent->flags);

    if (!memref->parent->buffer) {
        IMSG("no buffer");
        return TEEC_SUCCESS;
    }

    memcpy(param->value, memref->parent->buffer, param->val_len);

    HEXDUMP("", param->value, param->val_len);

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

    /* Create empty operation with TEEC_NONE params to be sent to TEE*/
    if (!operation) {
        IMSG("No params");
        op = calloc(1, sizeof(TEEC_Operation));
        if (!op) {
            EMSG("out of memory");
            err = TEEC_ERROR_OUT_OF_MEMORY;
            goto out;
        }
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

        param->param_type = TEEC_PARAM_TYPE_GET(op->paramTypes, i);

        switch (param_type) {
        case TEEC_NONE:
            IMSG("TEEC_NONE");
            param->val_len = 0;
            break;
        case TEEC_VALUE_INPUT:
        case TEEC_VALUE_OUTPUT:
        case TEEC_VALUE_INOUT:
            IMSG("TEEC_VALUE [%d]: a: 0x%x, b: 0x%x", param_type,
                op->params[i].value.a, op->params[i].value.b);

            param->val_len = sizeof(TEEC_Value);
            memcpy(param->value, &op->params[i].value, param->val_len);
            break;
        case TEEC_MEMREF_TEMP_INPUT:
        case TEEC_MEMREF_TEMP_OUTPUT:
        case TEEC_MEMREF_TEMP_INOUT:
            serialize_tmpref(&op->params[i].tmpref, param);
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
            EMSG("Unknown param type: %d", param_type);
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
                                      struct serialized_param *param)
{
    size_t tmpref_size = teec_param->tmpref.size;

    if (param_type != param->param_type) {
        EMSG("Invalid param type: %d / %d", param_type, param->param_type);
        return TEEC_ERROR_BAD_FORMAT;
    }

    IMSG("TEEC_MEMREF_TEMP [%d] len: %ld / %d", param_type,
        teec_param->tmpref.size, param->val_len);

    /* If provided buffer was too short TA might return required
     * buffer size back.
     */
    teec_param->tmpref.size = param->val_len;

    if (!teec_param->tmpref.buffer) {
        IMSG("memref NULL buffer");
        return TEEC_SUCCESS;
    }

    memcpy(teec_param->tmpref.buffer,
           param->value,
           MIN(tmpref_size, param->val_len));

    if (param->val_len > tmpref_size) {
        IMSG("partial copy: %ld / %d", tmpref_size, param->val_len);
    }

    HEXDUMP("", param->value, param->val_len);

    return TEEC_SUCCESS;
}

static TEEC_Result deserialize_memref(TEEC_Parameter *teec_param,
                                      struct serialized_param *param)
{
    if (!teec_param->memref.parent) {
        EMSG("invalid memref");
        return TEEC_ERROR_BAD_PARAMETERS;
    }

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

    IMSG("TEEC_MEMREF_WHOLE [%d] len: %ld / %d", param->param_type,
        teec_param->memref.parent->size, param->val_len);

    /* If provided buffer was too short TA might return required
     * buffer size back.
     */
    teec_param->memref.size = param->val_len;

    if (!teec_param->memref.parent->buffer) {
        IMSG("memref NULL buffer");
        return TEEC_SUCCESS;
    }

    memcpy(teec_param->memref.parent->buffer,
           param->value,
           MIN(teec_param->memref.parent->size, teec_param->memref.size));

    if (teec_param->memref.size > teec_param->memref.parent->size) {
        IMSG("partial copy: %ld / %ld", teec_param->memref.parent->size,
            teec_param->memref.size);
    }

    HEXDUMP("", param->value, param->val_len);

    return TEEC_SUCCESS;
}

static TEEC_Result deserialize_value(uint32_t param_type,
                                     TEEC_Parameter *teec_param,
                                     struct serialized_param *param)
{
    if (param_type != param->param_type) {
        EMSG("Invalid param type: %d / %d", param_type, param->param_type);
        return TEEC_ERROR_BAD_FORMAT;
    }

    if (param->val_len != sizeof(TEEC_Value)){
        EMSG("invalid param len: %d", param->val_len);
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    memcpy(&teec_param->value, param->value, param->val_len);

    IMSG("TEEC_VALUE [%d]: a: 0x%x, b: 0x%x", param_type, teec_param->value.a,
        teec_param->value.b);

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
        EMSG("Invalid params");
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    for (int i = 0; i < TEEC_CONFIG_PAYLOAD_REF_COUNT; i++) {
        if ((uintptr_t)param >= buf_end) {
            EMSG("Buffer overflow");
            err = TEEC_ERROR_EXCESS_DATA;
            goto out;
        }

        param_type = TEEC_PARAM_TYPE_GET(operation->paramTypes, i);

        switch (param_type) {
        case TEEC_NONE:
            IMSG("TEEC_NONE");
            break;
        case TEEC_VALUE_INPUT:
            IMSG("TEEC_VALUE_INPUT (NOP)");
            break;
        case TEEC_VALUE_OUTPUT:
        case TEEC_VALUE_INOUT:
            err = deserialize_value(param_type, &operation->params[i], param);
            if (err) {
                goto out;
            }
            break;
        case TEEC_MEMREF_TEMP_INPUT:
            IMSG("TEEC_MEMREF_TEMP_INPUT (NOP)");
            break;
        case TEEC_MEMREF_TEMP_OUTPUT:
        case TEEC_MEMREF_TEMP_INOUT:
            err = deserialize_tmpref(param_type, &operation->params[i], param);
            if (err) {
                goto out;
            }
            break;
        case TEEC_MEMREF_WHOLE:
            err = deserialize_memref(&operation->params[i], param);
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

    err = 0;
out:

    return err;
}
#pragma GCC diagnostic pop
