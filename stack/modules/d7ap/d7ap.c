/*! \file d7ap.c
 *
 *  \copyright (C) Copyright 2018 University of Antwerp and others (http://oss-7.cosys.be)
 *                 Copyright 2018 Cortus SA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *  \author philippe.nunes@cortus.com
 */

#include "d7ap.h"
#include "d7ap_stack.h"

#include "d7ap_fs.h"
#include "phy.h"
#include "hwradio.h"
#include "errors.h"
#include "debug.h"
#include "dae.h"
#include "MODULE_D7AP_defs.h"

#if defined(FRAMEWORK_LOG_ENABLED) && defined(MODULE_D7AP_LOG_ENABLED)
#include "log.h"
#define DPRINT(...) log_print_stack_string(LOG_STACK_D7AP, __VA_ARGS__)
#else
#define DPRINT(...)
#endif

#define D7A_SECURITY_HEADER_SIZE 5

d7ap_resource_desc_t registered_client[MODULE_D7AP_MAX_CLIENT_COUNT];
uint8_t registered_client_nb = 0;
uint8_t alp_client_id;
bool inited = false;

#ifdef MODULE_ALP
alp_interface_t d7_alp_interface;
alp_interface_config_t d7ap_session_config_buffer = (alp_interface_config_t) {
    .itf_id = ALP_ITF_ID_D7ASP
};
#endif // MODULE_ALP

#ifdef MODULE_ALP
void response_from_d7ap(uint16_t trans_id, uint8_t* payload, uint8_t len, d7ap_session_result_t result) {
    DPRINT("got response from d7 of trans %i with len %i and result linkbudget %i", trans_id, len, result.link_budget);

    alp_interface_status_t d7_status = (alp_interface_status_t) {
        .itf_id = ALP_ITF_ID_D7ASP,
        .len = 15 + d7ap_addressee_id_length(result.addressee.ctrl.id_type),
        .d7ap_session_result = result
    };

    alp_layer_received_response(trans_id, payload, len, &d7_status);
}

bool command_from_d7ap(uint8_t* payload, uint8_t len, d7ap_session_result_t result) {
    DPRINT("command from d7 with len %i result linkbudget %i", len, result.link_budget);
    alp_interface_status_t d7_status = (alp_interface_status_t) {
        .itf_id = ALP_ITF_ID_D7ASP,
        .len = 15 + d7ap_addressee_id_length(result.addressee.ctrl.id_type),
        .d7ap_session_result = result
    };

    return alp_layer_process_command(payload, len, ALP_ITF_ID_D7ASP, &d7_status);
}

error_t d7ap_alp_send(uint8_t* payload, uint8_t payload_length, uint8_t expected_response_length, uint16_t* trans_id, alp_interface_config_t* itf_cfg) {
    DPRINT("sending D7 packet");
    if(itf_cfg != NULL) {
        return d7ap_send(alp_client_id, (d7ap_session_config_t*)&itf_cfg->itf_config, payload, payload_length, expected_response_length, trans_id);
    } else {
        return d7ap_send(alp_client_id, NULL, payload, payload_length, expected_response_length, trans_id);
    }
}

void d7ap_command_completed(uint16_t trans_id, error_t error) {
    alp_layer_command_completed(trans_id, &error, NULL);
}
#endif // MODULE_ALP

void d7ap_init()
{
    if(inited)
        return;
    inited = true;

    d7ap_fs_init();

    // Initialize the D7AP stack
    d7ap_stack_init();
    registered_client_nb = 0;

#ifdef MODULE_ALP
    d7_alp_interface = (alp_interface_t) {
        .itf_id = 0xD7,
        .itf_cfg_len = sizeof(d7ap_session_config_t),
        .itf_status_len = sizeof(d7ap_session_result_t),
        .send_command = d7ap_alp_send,
        .init = (void (*)(alp_interface_config_t *))d7ap_init, //we do not use the session config in d7ap init
        .deinit = d7ap_stop,
        .unique = true
    };

    alp_layer_register_interface(&d7_alp_interface);

    d7ap_resource_desc_t alp_desc = {
      .receive_cb = response_from_d7ap,
      .transmitted_cb = d7ap_command_completed,
      .unsolicited_cb = command_from_d7ap
    };

    alp_client_id = d7ap_register(&alp_desc);

    DPRINT("alp_client_id is %i",alp_client_id);
#endif // MODULE_ALP
}

void d7ap_stop()
{
    inited = false;
    d7ap_stack_stop();
    registered_client_nb = 0;
}

/**
 * @brief   Register the client callbacks
 *
 * @param[in] desc pointer to the client resource
 *
 * @return  the client Id
 */
uint8_t d7ap_register(d7ap_resource_desc_t* desc)
{
    assert(registered_client_nb < D7AP_MAX_CLIENT_COUNT);
    registered_client[registered_client_nb] = *desc;
    registered_client_nb++;
    return (registered_client_nb-1);
}

//TODO to unregister, better to introduce a linked list for the registered clients
/*void d7ap_unregister(uint8_t client_id)
{
    assert(client_id < registered_client_nb);

}*/

/**
 * @brief   Gets the device address UID/VID
 *
 * @param[out] *addr   buffer to store the device addressee UID/VID.
 *                     the buffer should be large enough to contain the 64 bits UID
 * @return the address type (either UID or VID)
 */
d7ap_addressee_id_type_t d7ap_get_dev_addr(uint8_t* addr)
{
    d7ap_fs_read_vid(addr);

    // vid is not valid when set to FF
    if (memcmp(addr, (uint8_t[2]){ 0xFF, 0xFF }, 2) == 0)
    {
        d7ap_fs_read_uid(addr);
        return (ID_TYPE_UID);
    } else
        return (ID_TYPE_VID);
}


/**
 * @brief Get the maximum payload size according the security configuration.
 *
 * @param[in] nls_method     The security configuration.
 *
 * @returns the maximum payload size in bytes.
 */
uint8_t d7ap_get_payload_max_size(nls_method_t nls_method)
{
    uint8_t max_payload_size = D7A_PAYLOAD_MAX_SIZE;

    switch (nls_method)
    {
        case(AES_CTR):
            max_payload_size -= D7A_SECURITY_HEADER_SIZE;
            break;
        case(AES_CBC_MAC_128):
            max_payload_size -= 16;
            break;
        case(AES_CBC_MAC_64):
            max_payload_size -= 8;
            break;
        case(AES_CBC_MAC_32):
            max_payload_size -= 4;
            break;
        case(AES_CCM_128):
            max_payload_size -= 16 + D7A_SECURITY_HEADER_SIZE;
            break;
        case(AES_CCM_64):
            max_payload_size -= 8 + D7A_SECURITY_HEADER_SIZE;
            break;
        case(AES_CCM_32):
            max_payload_size -= 4 + D7A_SECURITY_HEADER_SIZE;
            break;
        case AES_NONE:
        default:
            break;
    }

    return max_payload_size;
}


/**
 * @brief   Send a packet over DASH7 network
 *
 * @param[in] clientID  The registered client Id
 * @param[in] config    The configuration for the d7a session. Set to NULL to use the current config
 * @param[in] payload   The pointer to the payload buffer
 * @param[in] len       The length of the payload
 * @param[in] expected_response_len The length of the expected response
 * @param[in,out] trans_id   Set the value of this parameter to NULL to cause the function to execute synchronously.
 *                           If this parameter is not NULL, the call executes asynchronously. Upon return from this function,
 *                           this points to the transaction identifier associated with the asynchronous operation.
 * @return 0 on success
 * @return an error (errno.h) in case of failure
 */
error_t d7ap_send(uint8_t client_id, d7ap_session_config_t* config, uint8_t* payload,
                  uint8_t len, uint8_t expected_response_len, uint16_t *trans_id)
{
    error_t error;

    if (client_id > registered_client_nb)
        return -ESIZE;

    error = d7ap_stack_send(client_id, config, payload, len, expected_response_len, trans_id);
    if (error > 0)
    {
        DPRINT("d7ap_stack_send failed with error %x", error);
    }

    return error;
}


/**
 * @brief   Sets the channels TX power index
 *
 * @param[in] power  The TX power index (from 1 to 16)
 */
void d7ap_set_tx_power(uint8_t power)
{
    hw_radio_set_tx_power(power);
}


/**
 * @brief   Gets the channels TX power index
 *
 * @return  The TX power index (from 1 to 16)
 */
uint8_t d7ap_get_tx_power(void)
{
    // TODO
    return 0;
}


/**
 * @brief   Sets the device access class
 *
 * @param[in] cls  The device access class
 */
void d7ap_set_access_class(uint8_t access_class)
{
    d7ap_fs_write_dll_conf_active_access_class(access_class);
}


/**
 * @brief   Gets the device access class
 *
 * @return  The device access class
 */
uint8_t d7ap_get_access_class(void)
{
    return d7ap_fs_read_dll_conf_active_access_class();
}
