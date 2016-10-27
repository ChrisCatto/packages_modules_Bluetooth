/******************************************************************************
 *
 *  Copyright (C) 1999-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  this file contains GATT authentication handling functions
 *
 ******************************************************************************/
#include "bt_target.h"
#include "bt_utils.h"

#if (BLE_INCLUDED == TRUE)
#include <string.h>
#include "bt_common.h"

#include "gatt_int.h"
#include "gatt_api.h"
#include "btm_int.h"

/*******************************************************************************
**
** Function         gatt_sign_data
**
** Description      This function sign the data for write command.
**
** Returns          true if encrypted, otherwise false.
**
*******************************************************************************/
static bool    gatt_sign_data (tGATT_CLCB *p_clcb)
{
    tGATT_VALUE         *p_attr = (tGATT_VALUE *)p_clcb->p_attr_buf;
    uint8_t             *p_data = NULL, *p;
    uint16_t            payload_size = p_clcb->p_tcb->payload_size;
    bool                status = false;
    uint8_t              *p_signature;

    /* do not need to mark channel securoty activity for data signing */
    gatt_set_sec_act(p_clcb->p_tcb, GATT_SEC_OK);

    p_data = (uint8_t *)osi_malloc(p_attr->len + 3); /* 3 = 2 byte handle + opcode */

    p = p_data;
    UINT8_TO_STREAM(p, GATT_SIGN_CMD_WRITE);
    UINT16_TO_STREAM(p, p_attr->handle);
    ARRAY_TO_STREAM(p, p_attr->value, p_attr->len);

    /* sign data length should be attribulte value length plus 2B handle + 1B op code */
    if ((payload_size - GATT_AUTH_SIGN_LEN - 3) < p_attr->len)
        p_attr->len = payload_size - GATT_AUTH_SIGN_LEN - 3;

    p_signature = p_attr->value + p_attr->len;
    if (BTM_BleDataSignature(p_clcb->p_tcb->peer_bda,
                             p_data,
                             (uint16_t)(p_attr->len + 3), /* 3 = 2 byte handle + opcode */
                             p_signature)) {
        p_attr->len += BTM_BLE_AUTH_SIGN_LEN;
        gatt_set_ch_state(p_clcb->p_tcb, GATT_CH_OPEN);
        gatt_act_write(p_clcb, GATT_SEC_SIGN_DATA);
    } else {
        gatt_end_operation(p_clcb, GATT_INTERNAL_ERROR, NULL);
    }

    osi_free(p_data);

    return status;
}

/*******************************************************************************
**
** Function         gatt_verify_signature
**
** Description      This function start to verify the sign data when receiving
**                  the data from peer device.
**
** Returns
**
*******************************************************************************/
void gatt_verify_signature(tGATT_TCB *p_tcb, BT_HDR *p_buf)
{
    uint16_t cmd_len;
    uint8_t op_code;
    uint8_t *p, *p_orig = (uint8_t *)(p_buf + 1) + p_buf->offset;
    uint32_t counter;

    if (p_buf->len < GATT_AUTH_SIGN_LEN + 4) {
        GATT_TRACE_ERROR("%s: Data length %u less than expected %u",
                         __func__, p_buf->len, GATT_AUTH_SIGN_LEN + 4);
        return;
    }
    cmd_len = p_buf->len - GATT_AUTH_SIGN_LEN + 4;
    p =  p_orig + cmd_len - 4;
    STREAM_TO_UINT32(counter, p);

    if (BTM_BleVerifySignature(p_tcb->peer_bda, p_orig, cmd_len, counter, p))
    {
        STREAM_TO_UINT8(op_code, p_orig);
        gatt_server_handle_client_req (p_tcb, op_code, (uint16_t)(p_buf->len - 1), p_orig);
    }
    else
    {
        /* if this is a bad signature, assume from attacker, ignore it  */
        GATT_TRACE_ERROR("Signature Verification Failed, data ignored");
    }

    return;
}
/*******************************************************************************
**
** Function         gatt_sec_check_complete
**
** Description      security check complete and proceed to data sending action.
**
** Returns          void.
**
*******************************************************************************/
void gatt_sec_check_complete(bool    sec_check_ok, tGATT_CLCB   *p_clcb, uint8_t sec_act)
{
    if (p_clcb && p_clcb->p_tcb &&
        fixed_queue_is_empty(p_clcb->p_tcb->pending_enc_clcb)) {
        gatt_set_sec_act(p_clcb->p_tcb, GATT_SEC_NONE);
    }

    if (!sec_check_ok)
    {
        gatt_end_operation(p_clcb, GATT_AUTH_FAIL, NULL);
    }
    else if (p_clcb->operation == GATTC_OPTYPE_WRITE)
    {
        gatt_act_write(p_clcb, sec_act);
    }
    else if (p_clcb->operation == GATTC_OPTYPE_READ)
    {
        gatt_act_read(p_clcb, p_clcb->counter);
    }
}
/*******************************************************************************
**
** Function         gatt_enc_cmpl_cback
**
** Description      link encryption complete callback.
**
** Returns
**
*******************************************************************************/
void gatt_enc_cmpl_cback(BD_ADDR bd_addr, tBT_TRANSPORT transport,
                         UNUSED_ATTR void *p_ref_data, tBTM_STATUS result)
{
    tGATT_TCB   *p_tcb;
    uint8_t     sec_flag;
    bool        status = false;

    GATT_TRACE_DEBUG("gatt_enc_cmpl_cback");
    if ((p_tcb = gatt_find_tcb_by_addr(bd_addr, transport)) != NULL)
    {
        if (gatt_get_sec_act(p_tcb) == GATT_SEC_ENC_PENDING)
            return;

        tGATT_PENDING_ENC_CLCB *p_buf =
            (tGATT_PENDING_ENC_CLCB *)fixed_queue_try_dequeue(p_tcb->pending_enc_clcb);
        if (p_buf != NULL)
        {
            if (result == BTM_SUCCESS)
            {
                if (gatt_get_sec_act(p_tcb) == GATT_SEC_ENCRYPT_MITM )
                {
                    BTM_GetSecurityFlagsByTransport(bd_addr, &sec_flag, transport);

                    if (sec_flag & BTM_SEC_FLAG_LKEY_AUTHED)
                    {
                        status = true;
                    }
                }
                else
                {
                    status = true;
                }
            }
            gatt_sec_check_complete(status, p_buf->p_clcb, p_tcb->sec_act);
            osi_free(p_buf);
            /* start all other pending operation in queue */
            for (size_t count = fixed_queue_length(p_tcb->pending_enc_clcb);
                 count > 0; count--)
            {
                p_buf = (tGATT_PENDING_ENC_CLCB *)fixed_queue_try_dequeue(p_tcb->pending_enc_clcb);
                if (p_buf != NULL)
                {
                    gatt_security_check_start(p_buf->p_clcb);
                    osi_free(p_buf);
                }
                else
                    break;
            }
        }
        else
        {
            GATT_TRACE_ERROR("Unknown operation encryption completed");
        }
    }
    else
    {
        GATT_TRACE_ERROR("enc callback for unknown bd_addr");
    }
}

/*******************************************************************************
**
** Function         gatt_notify_enc_cmpl
**
** Description      link encryption complete notification for all encryption process
**                  initiated outside GATT.
**
** Returns
**
*******************************************************************************/
void gatt_notify_enc_cmpl(BD_ADDR bd_addr)
{
    tGATT_TCB   *p_tcb;
    uint8_t      i = 0;

    if ((p_tcb = gatt_find_tcb_by_addr(bd_addr, BT_TRANSPORT_LE)) != NULL)
    {
        for (i = 0; i < GATT_MAX_APPS; i++)
        {
            if (gatt_cb.cl_rcb[i].in_use && gatt_cb.cl_rcb[i].app_cb.p_enc_cmpl_cb)
            {
                (*gatt_cb.cl_rcb[i].app_cb.p_enc_cmpl_cb)(gatt_cb.cl_rcb[i].gatt_if, bd_addr);
            }
        }

        if (gatt_get_sec_act(p_tcb) == GATT_SEC_ENC_PENDING)
        {
            gatt_set_sec_act(p_tcb, GATT_SEC_NONE);

            size_t count = fixed_queue_length(p_tcb->pending_enc_clcb);
            for (; count > 0; count--)
            {
                tGATT_PENDING_ENC_CLCB *p_buf =
                    (tGATT_PENDING_ENC_CLCB *)fixed_queue_try_dequeue(p_tcb->pending_enc_clcb);
                if (p_buf != NULL)
                {
                    gatt_security_check_start(p_buf->p_clcb);
                    osi_free(p_buf);
                }
                else
                    break;
            }
        }
    }
    else
    {
        GATT_TRACE_DEBUG("notify GATT for encryption completion of unknown device");
    }
    return;
}
/*******************************************************************************
**
** Function         gatt_set_sec_act
**
** Description      This function set the sec_act in clcb
**
** Returns          none
**
*******************************************************************************/
void gatt_set_sec_act(tGATT_TCB *p_tcb, tGATT_SEC_ACTION sec_act)
{
    if (p_tcb)
    {
        p_tcb->sec_act = sec_act;
    }
}
/*******************************************************************************
**
** Function         gatt_get_sec_act
**
** Description      This function get the sec_act in clcb
**
** Returns          none
**
*******************************************************************************/
tGATT_SEC_ACTION gatt_get_sec_act(tGATT_TCB *p_tcb)
{
    tGATT_SEC_ACTION sec_act = GATT_SEC_NONE;
    if (p_tcb)
    {
        sec_act = p_tcb->sec_act;
    }
    return sec_act;
}
/*******************************************************************************
**
** Function         gatt_determine_sec_act
**
** Description      This routine determine the security action based on auth_request and
**                  current link status
**
** Returns          tGATT_SEC_ACTION security action
**
*******************************************************************************/
tGATT_SEC_ACTION gatt_determine_sec_act(tGATT_CLCB *p_clcb )
{
    tGATT_SEC_ACTION    act = GATT_SEC_OK;
    uint8_t             sec_flag;
    tGATT_TCB           *p_tcb = p_clcb->p_tcb;
    tGATT_AUTH_REQ      auth_req = p_clcb->auth_req;
    bool                is_link_encrypted= false;
    bool                is_link_key_known=false;
    bool                is_key_mitm=false;
    uint8_t             key_type;
    tBTM_BLE_SEC_REQ_ACT    sec_act = BTM_LE_SEC_NONE;

    if (auth_req == GATT_AUTH_REQ_NONE )
        return act;

    BTM_GetSecurityFlagsByTransport(p_tcb->peer_bda, &sec_flag, p_clcb->p_tcb->transport);

    btm_ble_link_sec_check(p_tcb->peer_bda, auth_req, &sec_act);

    /* if a encryption is pending, need to wait */
    if (sec_act == BTM_BLE_SEC_REQ_ACT_DISCARD &&
        auth_req != GATT_AUTH_REQ_NONE)
        return GATT_SEC_ENC_PENDING;

    if (sec_flag & (BTM_SEC_FLAG_ENCRYPTED| BTM_SEC_FLAG_LKEY_KNOWN))
    {
        if (sec_flag & BTM_SEC_FLAG_ENCRYPTED)
            is_link_encrypted = true;

        is_link_key_known = true;

        if (sec_flag & BTM_SEC_FLAG_LKEY_AUTHED)
            is_key_mitm = true;
    }

    /* first check link key upgrade required or not */
    switch (auth_req)
    {
        case GATT_AUTH_REQ_MITM:
        case GATT_AUTH_REQ_SIGNED_MITM:
            if (!is_key_mitm)
                act = GATT_SEC_ENCRYPT_MITM;
            break;

        case GATT_AUTH_REQ_NO_MITM:
        case GATT_AUTH_REQ_SIGNED_NO_MITM:
            if (!is_link_key_known)
                act = GATT_SEC_ENCRYPT_NO_MITM;
            break;
        default:
            break;
    }

    /* now check link needs to be encrypted or not if the link key upgrade is not required */
    if (act == GATT_SEC_OK)
    {
        if (p_tcb->transport == BT_TRANSPORT_LE &&
            (p_clcb->operation == GATTC_OPTYPE_WRITE) &&
            (p_clcb->op_subtype == GATT_WRITE_NO_RSP))
        {
            /* this is a write command request
               check data signing required or not */
            if (!is_link_encrypted)
            {
                btm_ble_get_enc_key_type(p_tcb->peer_bda, &key_type);

                if ( (key_type & BTM_LE_KEY_LCSRK) &&
                     ((auth_req == GATT_AUTH_REQ_SIGNED_NO_MITM) ||
                      (auth_req == GATT_AUTH_REQ_SIGNED_MITM)))
                {
                    act = GATT_SEC_SIGN_DATA;
                }
                else
                {
                    act = GATT_SEC_ENCRYPT;
                }
            }
        }
        else
        {
            if (!is_link_encrypted)
            {
                act = GATT_SEC_ENCRYPT;
            }
        }

    }

    return  act ;

}



/*******************************************************************************
**
** Function         gatt_get_link_encrypt_status
**
** Description      This routine get the encryption status of the specified link
**
**
** Returns          tGATT_STATUS link encryption status
**
*******************************************************************************/
tGATT_STATUS gatt_get_link_encrypt_status(tGATT_TCB *p_tcb)
{
    tGATT_STATUS    encrypt_status = GATT_NOT_ENCRYPTED;
    uint8_t         sec_flag=0;

    BTM_GetSecurityFlagsByTransport(p_tcb->peer_bda, &sec_flag, p_tcb->transport);

    if ((sec_flag & BTM_SEC_FLAG_ENCRYPTED) && (sec_flag & BTM_SEC_FLAG_LKEY_KNOWN))
    {
        encrypt_status = GATT_ENCRYPED_NO_MITM;
        if (sec_flag & BTM_SEC_FLAG_LKEY_AUTHED)
            encrypt_status = GATT_ENCRYPED_MITM;
    }

    GATT_TRACE_DEBUG("gatt_get_link_encrypt_status status=0x%x",encrypt_status);
    return  encrypt_status ;
}


/*******************************************************************************
**
** Function          gatt_convert_sec_action
**
** Description      Convert GATT security action enum into equivalent BTM BLE security action enum
**
** Returns          bool    true - conversation is successful
**
*******************************************************************************/
static bool    gatt_convert_sec_action(tGATT_SEC_ACTION gatt_sec_act, tBTM_BLE_SEC_ACT *p_btm_sec_act )
{
    bool    status = true;
    switch (gatt_sec_act)
    {
        case GATT_SEC_ENCRYPT:
            *p_btm_sec_act = BTM_BLE_SEC_ENCRYPT;
            break;
        case GATT_SEC_ENCRYPT_NO_MITM:
            *p_btm_sec_act = BTM_BLE_SEC_ENCRYPT_NO_MITM;
            break;
        case GATT_SEC_ENCRYPT_MITM:
            *p_btm_sec_act = BTM_BLE_SEC_ENCRYPT_MITM;
            break;
        default:
            status = false;
            break;
    }

    return status;
}
/*******************************************************************************
**
** Function         gatt_check_enc_req
**
** Description      check link security.
**
** Returns          true if encrypted, otherwise false.
**
*******************************************************************************/
bool    gatt_security_check_start(tGATT_CLCB *p_clcb)
{
    tGATT_TCB           *p_tcb = p_clcb->p_tcb;
    tGATT_SEC_ACTION    gatt_sec_act;
    tBTM_BLE_SEC_ACT    btm_ble_sec_act;
    bool                status = true;
    tBTM_STATUS         btm_status;
    tGATT_SEC_ACTION    sec_act_old =  gatt_get_sec_act(p_tcb);

    gatt_sec_act = gatt_determine_sec_act(p_clcb);

    if (sec_act_old == GATT_SEC_NONE)
        gatt_set_sec_act(p_tcb, gatt_sec_act);

    switch (gatt_sec_act )
    {
        case GATT_SEC_SIGN_DATA:
            GATT_TRACE_DEBUG("gatt_security_check_start: Do data signing");
            gatt_sign_data(p_clcb);
            break;
        case GATT_SEC_ENCRYPT:
        case GATT_SEC_ENCRYPT_NO_MITM:
        case GATT_SEC_ENCRYPT_MITM:
            if (sec_act_old < GATT_SEC_ENCRYPT)
            {
                GATT_TRACE_DEBUG("gatt_security_check_start: Encrypt now or key upgreade first");
                gatt_convert_sec_action(gatt_sec_act, &btm_ble_sec_act);
                btm_status = BTM_SetEncryption(p_tcb->peer_bda, p_tcb->transport,
                                               gatt_enc_cmpl_cback, NULL, btm_ble_sec_act);
                if ( (btm_status != BTM_SUCCESS) && (btm_status != BTM_CMD_STARTED))
                {
                    GATT_TRACE_ERROR("gatt_security_check_start BTM_SetEncryption failed btm_status=%d", btm_status);
                    status = false;
                }
            }
            if (status)
                gatt_add_pending_enc_channel_clcb (p_tcb, p_clcb);
            break;
        case GATT_SEC_ENC_PENDING:
            gatt_add_pending_enc_channel_clcb (p_tcb, p_clcb);
            /* wait for link encrypotion to finish */
            break;
        default:
            gatt_sec_check_complete(true, p_clcb, gatt_sec_act);
            break;
    }

    if (status == false)
    {
        gatt_set_sec_act(p_tcb, GATT_SEC_NONE);
        gatt_set_ch_state(p_tcb, GATT_CH_OPEN);
    }

    return status;
}


#endif  /* BLE_INCLUDED */
