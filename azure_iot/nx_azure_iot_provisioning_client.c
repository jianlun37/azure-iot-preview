/**************************************************************************/
/*                                                                        */
/*       Copyright (c) Microsoft Corporation. All rights reserved.        */
/*                                                                        */
/*       This software is licensed under the Microsoft Software License   */
/*       Terms for Microsoft Azure RTOS. Full text of the license can be  */
/*       found in the LICENSE file at https://aka.ms/AzureRTOS_EULA       */
/*       and in the root directory of this software.                      */
/*                                                                        */
/**************************************************************************/

#include "nx_azure_iot_provisioning_client.h"

#include "az_span.h"

/* Define AZ IoT Provisioning Client state.  */
#define NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_NONE                  0
#define NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_INIT                  1
#define NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_CONNECT               2
#define NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_SUBSCRIBE             3
#define NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_REQUEST               4
#define NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_WAITING_FOR_RESPONSE  5
#define NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_DONE                  6
#define NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_ERROR                 7

/* Define AZ IoT Provisioning Client topic format. */
#define NX_AZURE_IOT_PROVISIONING_CLIENT_REG_SUB_TOPIC                "$dps/registrations/res/#"
#define NX_AZURE_IOT_PROVISIONING_CLIENT_PAYLOAD_START                "{\"registrationId\" : \""
#define NX_AZURE_IOT_PROVISIONING_CLIENT_PAYLOAD_END                  "\"}"
#define NX_AZURE_IOT_PROVISIONING_CLIENT_POLICY_NAME                  "registration"

/* Set the default timeout for connecting on cloud thread. */
#ifndef NX_AZURE_IOT_PROVISIONING_CLIENT_CONNECT_TIMEOUT
#define NX_AZURE_IOT_PROVISIONING_CLIENT_CONNECT_TIMEOUT              (20 * NX_IP_PERIODIC_RATE)
#endif /* NX_AZURE_IOT_PROVISIONING_CLIENT_CONNECT_TIMEOUT */

/* Set the default retry to Provisioning service. */
#ifndef NX_AZURE_IOT_PROVISIONING_CLIENT_DEFAULT_RETRY
#define NX_AZURE_IOT_PROVISIONING_CLIENT_DEFAULT_RETRY                (3)
#endif /* NX_AZURE_IOT_PROVISIONING_CLIENT_DEFAULT_RETRY */

static UINT nx_azure_iot_provisioning_client_connect_internal(NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr,
                                                              UINT wait_option);
static VOID nx_azure_iot_provisioning_client_mqtt_receive_callback(NXD_MQTT_CLIENT *client_ptr,
                                                                   UINT number_of_messages);
static VOID nx_azure_iot_provisioning_client_mqtt_disconnect_notify(NXD_MQTT_CLIENT *client_ptr);
static UINT nx_azure_iot_provisioning_client_send_req(NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr,
                                                      az_iot_provisioning_client_register_response const *register_response,
                                                      UINT wait_option);
static VOID nx_azure_iot_provisioning_client_event_process(NX_AZURE_IOT *nx_azure_iot_ptr,
                                                           ULONG common_events, ULONG module_own_events);
static VOID nx_azure_iot_provisioning_client_update_state(NX_AZURE_IOT_PROVISIONING_CLIENT *context, UINT action_result);

extern UINT _nxd_mqtt_process_publish_packet(NX_PACKET *packet_ptr, ULONG *topic_offset_ptr, USHORT *topic_length_ptr,
                                             ULONG *message_offset_ptr, ULONG *message_length_ptr);
extern UINT _nx_cloud_module_event_set(NX_CLOUD_MODULE *cloud_module, ULONG module_own_event);

static UINT nx_azure_iot_provisioning_client_process_message(NX_AZURE_IOT_PROVISIONING_CLIENT *context, NX_PACKET *packet_ptr,
                                                             NX_AZURE_IOT_PROVISIONING_RESPONSE *response)
{
ULONG topic_offset;
USHORT topic_length;
ULONG message_offset;
ULONG message_length;
az_span received_topic;
az_span received_payload;
az_result core_result;
UINT status;

    status = _nxd_mqtt_process_publish_packet(packet_ptr, &topic_offset, &topic_length,
                                              &message_offset, &message_length);
    if (status)
    {
        nx_packet_release(packet_ptr);
        return(status);
    }

    /* TODO: Support chained packet */
    if ((ULONG)(packet_ptr -> nx_packet_append_ptr - packet_ptr -> nx_packet_prepend_ptr) <
        (message_offset + message_length))
    {
        LogError("IoTProvisioning client failed to parse chained packet");
        return(NX_AZURE_IOT_MESSAGE_TOO_LONG);
    }

    received_topic = az_span_init(packet_ptr -> nx_packet_prepend_ptr + topic_offset, (INT)topic_length);
    received_payload = az_span_init(packet_ptr -> nx_packet_prepend_ptr + message_offset, (INT)message_length);
    core_result =
      az_iot_provisioning_client_parse_received_topic_and_payload(&(context -> nx_azure_iot_provisioning_client_core),
                                                                  received_topic, received_payload,
                                                                  &response -> register_response);
    if (az_failed(core_result))
    {
        LogError("IoTProvisioning client failed to parse packet, error: 0x%08x", core_result);
        return(NX_AZURE_IOT_SDK_CORE_ERROR);
    }

    response -> packet_ptr = packet_ptr;

    return(NX_AZURE_IOT_SUCCESS);
}

static VOID nx_azure_iot_provisioning_client_mqtt_disconnect_notify(NXD_MQTT_CLIENT *client_ptr)
{
UINT status;
NX_AZURE_IOT_RESOURCE *resource = nx_azure_iot_resource_search(client_ptr);
NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr = NX_NULL;

    /* This function is protected by MQTT mutex. */

    if (resource && (resource -> resource_type == NX_AZURE_IOT_RESOURCE_IOT_PROVISIONING))
    {
        prov_client_ptr = (NX_AZURE_IOT_PROVISIONING_CLIENT *)resource -> resource_data_ptr;
    }

    /* Set disconnect event.  */
    if (prov_client_ptr)
    {
        status = nx_cloud_module_event_set(&(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_cloud_module),
                                           NX_AZURE_IOT_PROVISIONING_CLIENT_DISCONNECT_EVENT);
        if (status)
        {
            nx_azure_iot_provisioning_client_update_state(prov_client_ptr, status);
        }
    }
}

static UINT nx_azure_iot_provisioning_client_connect_internal(NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr,
                                                              UINT wait_option)
{

UINT status;
NXD_ADDRESS server_address;
NXD_MQTT_CLIENT *mqtt_client_ptr;
UINT dns_timeout = wait_option;
NX_AZURE_IOT_RESOURCE *resource_ptr;

    /* Set the DNS timeout as NX_AZURE_IOT_PROVISIONING_CLIENT_DNS_TIMEOUT for non-blocking mode.  */ /* TODO: DNS non-blocking.  */
    if (dns_timeout == 0)
    {
        dns_timeout = NX_AZURE_IOT_PROVISIONING_CLIENT_DNS_TIMEOUT;
    }

    /* Resolve the host name.  */
    /* TODO: Once non-block dns resolve is implemented update this */
    status = nxd_dns_host_by_name_get(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_dns_ptr,
                                      prov_client_ptr -> nx_azure_iot_provisioning_client_endpoint,
                                      &server_address, dns_timeout, NX_IP_VERSION_V4);
    if (status)
    {
        LogError("IoTProvisioning client connect fail: DNS RESOLVE FAIL: 0x%02x", status);
        return(status);
    }

    /* Set MQTT Client.  */
    resource_ptr = &(prov_client_ptr -> nx_azure_iot_provisioning_client_resource);
    mqtt_client_ptr = &(resource_ptr -> resource_mqtt);

    /* Set login info.  */
    status = nxd_mqtt_client_login_set(mqtt_client_ptr, (CHAR *)resource_ptr -> resource_mqtt_user_name,
                                       resource_ptr -> resource_mqtt_user_name_length,
                                       (CHAR *)resource_ptr -> resource_mqtt_sas_token,
                                       resource_ptr -> resource_mqtt_sas_token_length);
    if (status)
    {
        LogError("IoTProvisioning client connect fail: MQTT CLIENT LOGIN SET FAIL: 0x%02x", status);
        return(status);
    }

    /* Start MQTT connection.  */
    status = nxd_mqtt_client_secure_connect(mqtt_client_ptr, &server_address, NXD_MQTT_TLS_PORT,
                                            nx_azure_iot_mqtt_tls_setup, NX_AZURE_IOT_MQTT_KEEP_ALIVE,
                                            NX_FALSE, wait_option);

    if ((wait_option == NX_NO_WAIT) && (status == NX_IN_PROGRESS))
    {
        LogInfo("IoTProvisioning client connect pending");
        return(NX_AZURE_IOT_SUCCESS);
    }

    /* Check status.  */
    if (status != NX_AZURE_IOT_SUCCESS)
    {
        LogError("IoTProvisioning client connect fail: MQTT CONNECT FAIL: 0x%02x", status);
        return(status);
    }

    return(NX_AZURE_IOT_SUCCESS);
}

static VOID nx_azure_iot_provisioning_client_mqtt_receive_callback(NXD_MQTT_CLIENT *client_ptr,
                                                                   UINT number_of_messages)
{
NX_AZURE_IOT_RESOURCE *resource = nx_azure_iot_resource_search(client_ptr);
NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr = NX_NULL;
NX_PACKET *packet_ptr;
NX_PACKET *packet_next_ptr;
UINT status;

    /* This function is protected by MQTT mutex. */

    NX_PARAMETER_NOT_USED(number_of_messages);

    if (resource && (resource -> resource_type == NX_AZURE_IOT_RESOURCE_IOT_PROVISIONING))
    {
        prov_client_ptr = (NX_AZURE_IOT_PROVISIONING_CLIENT *)resource -> resource_data_ptr;
    }

    if (prov_client_ptr)
    {
        for (packet_ptr = client_ptr -> message_receive_queue_head;
            packet_ptr;
            packet_ptr = packet_next_ptr)
        {

            /* Store next packet in case current packet is consumed. */
            packet_next_ptr = packet_ptr -> nx_packet_queue_next;

            /* Adjust packet to simply process logic. */
            nx_azure_iot_mqtt_packet_adjust(packet_ptr);

            /* Last response was not yet consumed, probably duplicate from service */
            if (prov_client_ptr -> nx_azure_iot_provisioning_client_last_response)
            {
                nx_packet_release(packet_ptr);
                continue;
            }

            prov_client_ptr -> nx_azure_iot_provisioning_client_last_response = packet_ptr;
            status = nx_cloud_module_event_set(&(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_cloud_module),
                                               NX_AZURE_IOT_PROVISIONING_CLIENT_RESPONSE_EVENT);
            if (status)
            {
                nx_azure_iot_provisioning_client_update_state(prov_client_ptr, status);
            }
        }

        /* Clear all message from MQTT receive queue. */
        client_ptr -> message_receive_queue_head = NX_NULL;
        client_ptr -> message_receive_queue_tail = NX_NULL;
        client_ptr -> message_receive_queue_depth = 0;
    }
}

/**
 *  State transitions :
 *      INIT -> {CONNECT|ERROR} -> {REQUEST|ERROR} -> {WAITING_FOR_REPONSE|ERROR} -> {DONE|REQUEST|ERROR}
 **/
static VOID nx_azure_iot_provisioning_client_update_state(NX_AZURE_IOT_PROVISIONING_CLIENT *context, UINT action_result)
{
UINT state = context -> nx_azure_iot_provisioning_client_state;
NX_AZURE_IOT_PROVISIONING_THREAD *thread_list_ptr;

    LogDebug("Action result in state [%d]: %d \r\n", state, action_result);

    context -> nx_azure_iot_provisioning_client_result = action_result;

    if (action_result == NX_AZURE_IOT_PENDING)
    {
        switch (state)
        {
            case NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_INIT :
            {
                context -> nx_azure_iot_provisioning_client_state = NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_CONNECT;
            }
            break;

            case NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_CONNECT :
            {
                context -> nx_azure_iot_provisioning_client_state = NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_SUBSCRIBE;
            }
            break;

            case NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_SUBSCRIBE :
            {
                context -> nx_azure_iot_provisioning_client_state = NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_REQUEST;
            }
            break;

            case NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_REQUEST :
            {
                context -> nx_azure_iot_provisioning_client_state = NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_WAITING_FOR_RESPONSE;
            }
            break;

            case NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_WAITING_FOR_RESPONSE :
            {
                context -> nx_azure_iot_provisioning_client_state = NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_REQUEST;
            }
            break;

            default :
            {
                LogError("Unknown state %d\r\n", state);
            }
            break;
        }
    }
    else
    {
        if (action_result == NX_AZURE_IOT_SUCCESS)
        {
            context -> nx_azure_iot_provisioning_client_state = NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_DONE;
        }
        else
        {
            context -> nx_azure_iot_provisioning_client_state = NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_ERROR;
        }

        /* Wake up all threads */
        for (thread_list_ptr = context -> nx_azure_iot_provisioning_client_thread_suspended;
             thread_list_ptr;
             thread_list_ptr = thread_list_ptr -> thread_next)
        {
            tx_thread_wait_abort(thread_list_ptr -> thread_ptr);
        }

        /* Delete the list */
        context -> nx_azure_iot_provisioning_client_thread_suspended = NULL;

        /* notify completion if required */
        if (context -> nx_azure_iot_provisioning_client_on_complete_callback)
        {
            context -> nx_azure_iot_provisioning_client_on_complete_callback(context, context -> nx_azure_iot_provisioning_client_result);
        }
    }
}

static VOID nx_azure_iot_provisioning_client_mqtt_connect_notify(struct NXD_MQTT_CLIENT_STRUCT *client_ptr, UINT status, VOID *context)
{

NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr = (NX_AZURE_IOT_PROVISIONING_CLIENT*)context;

    NX_PARAMETER_NOT_USED(client_ptr);

    /* mutex might got deleted by deinitalization */
    if (tx_mutex_get(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr, NX_WAIT_FOREVER))
    {
        return;
    }

    /* Update hub client status.  */
    if (status == NXD_MQTT_SUCCESS)
    {
        if (prov_client_ptr -> nx_azure_iot_provisioning_client_state == NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_CONNECT)
        {
            nx_azure_iot_provisioning_client_update_state(prov_client_ptr, NX_AZURE_IOT_PENDING);
            status = nx_cloud_module_event_set(&(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_cloud_module),
                                               NX_AZURE_IOT_PROVISIONING_CLIENT_SUBSCRIBE_EVENT);
            if (status)
            {
                nx_azure_iot_provisioning_client_update_state(prov_client_ptr, status);
            }
        }
    }
    else
    {
        status = nx_cloud_module_event_set(&(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_cloud_module),
                                           NX_AZURE_IOT_PROVISIONING_CLIENT_DISCONNECT_EVENT);
        if (status)
        {
            nx_azure_iot_provisioning_client_update_state(prov_client_ptr, status);
        }
    }
    tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);
}

static VOID nx_azure_iot_provisioning_client_process_connect(NX_AZURE_IOT_PROVISIONING_CLIENT *context)
{
UINT status;

    if ((context == NX_NULL) || (context -> nx_azure_iot_ptr == NX_NULL))
    {
        return;
    }


    /* Check the state.  */
    if (context -> nx_azure_iot_provisioning_client_state == NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_CONNECT)
    {
        context -> nx_azure_iot_provisioning_client_resource.resource_mqtt.nxd_mqtt_connect_notify = nx_azure_iot_provisioning_client_mqtt_connect_notify;
        context -> nx_azure_iot_provisioning_client_resource.resource_mqtt.nxd_mqtt_connect_context = context;

        /* Start connect.  */
        status = nx_azure_iot_provisioning_client_connect_internal(context, NX_NO_WAIT);

        if (status)
        {
            nx_azure_iot_provisioning_client_update_state(context, status);
        }
    }
}

static VOID nx_azure_iot_provisioning_client_process_timer(NX_AZURE_IOT_PROVISIONING_CLIENT *context)
{
UINT status;

    if (context == NULL)
    {
        return;
    }

    if (context -> nx_azure_iot_provisioning_client_req_timeout == 0)
    {
        return;
    }

    /* Trigger Request  */
    if (context -> nx_azure_iot_provisioning_client_req_timeout == 1)
    {

        /* TODO: check with Bo why we can't generate events from system thread. */
        status = _nx_cloud_module_event_set(&(context -> nx_azure_iot_ptr -> nx_azure_iot_cloud_module),
                                            NX_AZURE_IOT_PROVISIONING_CLIENT_REQUEST_EVENT);
        if (status)
        {
            nx_azure_iot_provisioning_client_update_state(context, status);
        }
    }

    context -> nx_azure_iot_provisioning_client_req_timeout--;
}

static VOID nx_azure_iot_provisioning_client_subscribe(NX_AZURE_IOT_PROVISIONING_CLIENT *context)
{
UINT status;

    if (context == NULL)
    {
        return;
    }

    /* Check the state.  */
    if (context -> nx_azure_iot_provisioning_client_state == NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_SUBSCRIBE)
    {

        /* Subscribe topic. */
        status = nxd_mqtt_client_subscribe(&(context -> nx_azure_iot_provisioning_client_resource.resource_mqtt),
                                           NX_AZURE_IOT_PROVISIONING_CLIENT_REG_SUB_TOPIC,
                                           sizeof(NX_AZURE_IOT_PROVISIONING_CLIENT_REG_SUB_TOPIC) - 1, 0);

        if (status)
        {
            nx_azure_iot_provisioning_client_update_state(context, status);
        }
        else
        {
            nx_azure_iot_provisioning_client_update_state(context, NX_AZURE_IOT_PENDING);
            status = nx_cloud_module_event_set(&(context -> nx_azure_iot_ptr -> nx_azure_iot_cloud_module),
                                               NX_AZURE_IOT_PROVISIONING_CLIENT_REQUEST_EVENT);
            if (status)
            {
                nx_azure_iot_provisioning_client_update_state(context, status);
            }
        }
    }
}

static VOID nx_azure_iot_provisioning_client_generate_service_request(NX_AZURE_IOT_PROVISIONING_CLIENT *context)
{
UINT status;

    if (context == NX_NULL)
    {
        return;
    }

    /* Check the state.  */
    if (context -> nx_azure_iot_provisioning_client_state == NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_REQUEST)
    {
        if (context -> nx_azure_iot_provisioning_client_response.packet_ptr)
        {

            /* Request status of existing operationId */
            status = nx_azure_iot_provisioning_client_send_req(context,
                                                               &(context -> nx_azure_iot_provisioning_client_response.register_response),
                                                               NX_NO_WAIT);
            nx_packet_release(context -> nx_azure_iot_provisioning_client_response.packet_ptr);

            context -> nx_azure_iot_provisioning_client_response.packet_ptr = NULL;
        }
        else
        {

            /* Start new operation */
            status = nx_azure_iot_provisioning_client_send_req(context, NULL, NX_AZURE_IOT_PROVISIONING_CLIENT_CONNECT_TIMEOUT);
        }

        if (status)
        {
            nx_azure_iot_provisioning_client_update_state(context, status);
        }
        else
        {
            nx_azure_iot_provisioning_client_update_state(context, NX_AZURE_IOT_PENDING);
        }
    }
}

static VOID nx_azure_iot_provisioning_client_process_service_response(NX_AZURE_IOT_PROVISIONING_CLIENT *context)
{
NX_PACKET *packet_ptr;
az_iot_provisioning_client_register_response *response;

    if (context == NX_NULL)
    {
        return;
    }

    /* Check the state.  */
    if (context -> nx_azure_iot_provisioning_client_state == NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_WAITING_FOR_RESPONSE)
    {

        packet_ptr = context -> nx_azure_iot_provisioning_client_last_response;
        context -> nx_azure_iot_provisioning_client_last_response = NULL;

        context -> nx_azure_iot_provisioning_client_result =
            nx_azure_iot_provisioning_client_process_message(context, packet_ptr,
                                                             &context -> nx_azure_iot_provisioning_client_response);
        if (context -> nx_azure_iot_provisioning_client_result)
        {
            nx_packet_release(packet_ptr);
            nx_azure_iot_provisioning_client_update_state(context, context -> nx_azure_iot_provisioning_client_result);
            return;
        }

        response = &(context -> nx_azure_iot_provisioning_client_response.register_response);
        if (az_span_is_content_equal(response -> operation_status, AZ_SPAN_FROM_STR("assigned")))
        {
            nx_azure_iot_provisioning_client_update_state(context, NX_AZURE_IOT_SUCCESS);
        }
        else if (response -> retry_after_seconds == 0)
        {

            /* Server responded with error with no retry */
            nx_azure_iot_provisioning_client_update_state(context, NX_AZURE_IOT_SERVER_RESPONSE_ERROR);
        }
        else
        {
            nx_azure_iot_provisioning_client_update_state(context, NX_AZURE_IOT_PENDING);
            context -> nx_azure_iot_provisioning_client_req_timeout = response -> retry_after_seconds;
        }
    }
}

static VOID nx_azure_iot_provisioning_client_process_disconnect(NX_AZURE_IOT_PROVISIONING_CLIENT *context)
{

    if (context == NX_NULL)
    {
        return;
    }

    /* Check the state and only allow disconnect event to be processed in non-complete state  */
    if (context -> nx_azure_iot_provisioning_client_state > NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_INIT &&
        context -> nx_azure_iot_provisioning_client_state < NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_DONE)
    {
        nx_azure_iot_provisioning_client_update_state(context, NX_AZURE_IOT_DISCONNECTED);
    }
}

static UINT nx_azure_iot_provisioning_client_send_req(NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr,
                                                      az_iot_provisioning_client_register_response const *register_response,
                                                      UINT wait_option)
{
NX_PACKET *packet_ptr;
UCHAR *buffer_ptr;
UINT buffer_size;
UCHAR packet_id[2];
UINT status;
UINT mqtt_topic_length;
az_result core_result;

    status = nx_azure_iot_publish_packet_get(prov_client_ptr -> nx_azure_iot_ptr,
                                             &prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_mqtt,
                                             &packet_ptr, wait_option);

    if (status)
    {
        LogError("IoTProvisioning request buffer creation failed");
        return(status);
    }

    buffer_ptr = packet_ptr -> nx_packet_prepend_ptr;
    buffer_size = (UINT)(packet_ptr -> nx_packet_data_end - packet_ptr -> nx_packet_prepend_ptr);

    status = nx_azure_iot_mqtt_packet_id_get(&(prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_mqtt),
                                             packet_id, wait_option);
    if (status)
    {
        LogError("failed to get packetId ");
        nx_packet_release(packet_ptr);
        return(status);
    }

    if (register_response == NULL)
    {
        core_result = az_iot_provisioning_client_register_get_publish_topic(&(prov_client_ptr -> nx_azure_iot_provisioning_client_core),
                                                                            (CHAR *)buffer_ptr, buffer_size, &mqtt_topic_length);
    }
    else
    {
        core_result = az_iot_provisioning_client_query_status_get_publish_topic(&(prov_client_ptr -> nx_azure_iot_provisioning_client_core),
                                                                                register_response, (CHAR *)buffer_ptr, buffer_size, &mqtt_topic_length);
    }

    if (az_failed(core_result))
    {
        LogError("failed to get topic, error: 0x%08x", core_result);
        nx_packet_release(packet_ptr);
        return(NX_AZURE_IOT_SDK_CORE_ERROR);
    }

    packet_ptr -> nx_packet_append_ptr = packet_ptr -> nx_packet_prepend_ptr + mqtt_topic_length;
    packet_ptr -> nx_packet_length += mqtt_topic_length;

    status = nx_packet_data_append(packet_ptr, packet_id, sizeof(packet_id),
                                   packet_ptr -> nx_packet_pool_owner,
                                   wait_option);
    if (status)
    {
        LogError("failed to append data");
        nx_packet_release(packet_ptr);
        return(status);
    }

    status = nx_packet_data_append(packet_ptr, NX_AZURE_IOT_PROVISIONING_CLIENT_PAYLOAD_START,
                                   sizeof(NX_AZURE_IOT_PROVISIONING_CLIENT_PAYLOAD_START) - 1,
                                   packet_ptr -> nx_packet_pool_owner,
                                   wait_option);
    if (status)
    {
        LogError("failed to append data");
        nx_packet_release(packet_ptr);
        return(status);
    }

    status = nx_packet_data_append(packet_ptr, prov_client_ptr -> nx_azure_iot_provisioning_client_registration_id,
                                   prov_client_ptr -> nx_azure_iot_provisioning_client_registration_id_length,
                                   packet_ptr -> nx_packet_pool_owner,
                                   wait_option);
    if (status)
    {
        LogError("failed to append data ");
        nx_packet_release(packet_ptr);
        return(status);
    }

    status = nx_packet_data_append(packet_ptr, NX_AZURE_IOT_PROVISIONING_CLIENT_PAYLOAD_END,
                                   sizeof(NX_AZURE_IOT_PROVISIONING_CLIENT_PAYLOAD_END) - 1,
                                   packet_ptr -> nx_packet_pool_owner,
                                   wait_option);
    if (status)
    {
        LogError("failed to append data ");
        nx_packet_release(packet_ptr);
        return(status);
    }

    status = nx_azure_iot_publish_mqtt_packet(&(prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_mqtt), packet_ptr,
                                              mqtt_topic_length, packet_id, NX_AZURE_IOT_MQTT_QOS_1, wait_option);

    if (status)
    {
        LogError("failed to publish packet");
        nx_packet_release(packet_ptr);
        return(status);
    }

    return(NX_AZURE_IOT_SUCCESS);
}

static VOID nx_azure_iot_provisioning_client_thread_dequeue(NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr,
                                                            NX_AZURE_IOT_PROVISIONING_THREAD *node)
{
NX_AZURE_IOT_PROVISIONING_THREAD *thread_list_ptr;
NX_AZURE_IOT_PROVISIONING_THREAD *thread_list_prev = NX_NULL;

    for (thread_list_ptr = prov_client_ptr -> nx_azure_iot_provisioning_client_thread_suspended;
         thread_list_ptr;
         thread_list_prev = thread_list_ptr, thread_list_ptr = thread_list_ptr -> thread_next)
    {
        if (thread_list_ptr != node)
        {
            continue;
        }

        if (thread_list_prev == NX_NULL)
        {
            prov_client_ptr -> nx_azure_iot_provisioning_client_thread_suspended = thread_list_ptr -> thread_next;
        }
        else
        {
            thread_list_prev -> thread_next = thread_list_ptr -> thread_next;
        }

        break;
    }
}

static UINT nx_azure_iot_provisioning_client_sas_token_get(NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr,
                                                           ULONG expiry_time_secs)
{
UCHAR *buffer_ptr;
UINT buffer_size;
VOID *buffer_context;
UINT status;
NX_AZURE_IOT_RESOURCE *resource_ptr;
UCHAR *output_ptr;
UINT output_len;
az_span span;
az_result core_result;
az_span buffer_span;
az_span policy_name = AZ_SPAN_LITERAL_FROM_STR(NX_AZURE_IOT_PROVISIONING_CLIENT_POLICY_NAME);

    resource_ptr = &(prov_client_ptr -> nx_azure_iot_provisioning_client_resource);
    span = az_span_init(resource_ptr -> resource_mqtt_sas_token,
                        (INT)prov_client_ptr -> nx_azure_iot_provisioning_client_sas_token_buff_size);

    status = nx_azure_iot_buffer_allocate(prov_client_ptr -> nx_azure_iot_ptr,
                                          &buffer_ptr, &buffer_size,
                                          &buffer_context);
    if (status)
    {
        LogError("IoTProvisioning client connect fail: BUFFER ALLOCATE FAIL");
        return(status);
    }

    core_result = az_iot_provisioning_client_sas_get_signature(&(prov_client_ptr -> nx_azure_iot_provisioning_client_core),
                                                               expiry_time_secs, span, &span);

    if (az_failed(core_result))
    {
        LogError("IoTProvisioning failed failed to get signature with error : 0x%08x", core_result);
        nx_azure_iot_buffer_free(buffer_context);
        return(NX_AZURE_IOT_SDK_CORE_ERROR);
    }

    status = nx_azure_iot_url_encoded_hmac_sha256_calculate(resource_ptr,
                                                            prov_client_ptr -> nx_azure_iot_provisioning_client_symmetric_key,
                                                            prov_client_ptr -> nx_azure_iot_provisioning_client_symmetric_key_length,
                                                            az_span_ptr(span), (UINT)az_span_size(span), buffer_ptr, buffer_size,
                                                            &output_ptr, &output_len);
    if (status)
    {
        LogError("IoTProvisioning failed to encoded hash");
        nx_azure_iot_buffer_free(buffer_context);
        return(status);
    }

    buffer_span = az_span_init(output_ptr, (INT)output_len);
    core_result = az_iot_provisioning_client_sas_get_password(&(prov_client_ptr -> nx_azure_iot_provisioning_client_core),
                                                              buffer_span, expiry_time_secs, policy_name,
                                                              (CHAR *)resource_ptr -> resource_mqtt_sas_token,
                                                              prov_client_ptr -> nx_azure_iot_provisioning_client_sas_token_buff_size,
                                                              &(resource_ptr -> resource_mqtt_sas_token_length));
    if (az_failed(core_result))
    {
        LogError("IoTProvisioning failed to generate token with error : 0x%08x", core_result);
        nx_azure_iot_buffer_free(buffer_context);
        return(NX_AZURE_IOT_SDK_CORE_ERROR);
    }

    nx_azure_iot_buffer_free(buffer_context);

    return(NX_AZURE_IOT_SUCCESS);
}

/* Define the prototypes for Azure RTOS IoT.  */
UINT nx_azure_iot_provisioning_client_initialize(NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr,
                                                 NX_AZURE_IOT *nx_azure_iot_ptr,
                                                 UCHAR *endpoint, UINT endpoint_length,
                                                 UCHAR *id_scope, UINT id_scope_length,
                                                 UCHAR *registration_id, UINT registration_id_length,
                                                 const NX_CRYPTO_METHOD **crypto_array, UINT crypto_array_size,
                                                 const NX_CRYPTO_CIPHERSUITE **cipher_map, UINT cipher_map_size,
                                                 UCHAR *metadata_memory, UINT memory_size,
                                                 NX_SECURE_X509_CERT *trusted_certificate)
{
UINT status;
UINT mqtt_user_name_length;
NXD_MQTT_CLIENT *mqtt_client_ptr;
NX_AZURE_IOT_RESOURCE *resource_ptr;
UCHAR *buffer_ptr;
UINT buffer_size;
VOID *buffer_context;
az_span endpoint_span = az_span_init(endpoint, (INT)endpoint_length);
az_span id_scope_span = az_span_init(id_scope, (INT)id_scope_length);
az_span registration_id_span = az_span_init(registration_id, (INT)registration_id_length);

    if ((nx_azure_iot_ptr == NX_NULL) || (prov_client_ptr == NX_NULL) || (endpoint == NX_NULL) ||
        (id_scope == NX_NULL) || registration_id == NX_NULL)
    {
        LogError("IoTProvisioning client create fail: INVALID POINTER");
        return(NX_AZURE_IOT_INVALID_PARAMETER);
    }

    /* Obtain the mutex.   */
    tx_mutex_get(nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr, TX_WAIT_FOREVER);

    memset(prov_client_ptr, 0, sizeof(NX_AZURE_IOT_PROVISIONING_CLIENT));

    /* Set resource pointer.  */
    resource_ptr = &(prov_client_ptr -> nx_azure_iot_provisioning_client_resource);
    mqtt_client_ptr = &(prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_mqtt);

    prov_client_ptr -> nx_azure_iot_ptr = nx_azure_iot_ptr;
    prov_client_ptr -> nx_azure_iot_provisioning_client_endpoint = endpoint;
    prov_client_ptr -> nx_azure_iot_provisioning_client_endpoint_length = endpoint_length;
    prov_client_ptr -> nx_azure_iot_provisioning_client_id_scope = id_scope;
    prov_client_ptr -> nx_azure_iot_provisioning_client_id_scope_length = id_scope_length;
    prov_client_ptr -> nx_azure_iot_provisioning_client_registration_id = registration_id;
    prov_client_ptr -> nx_azure_iot_provisioning_client_registration_id_length = registration_id_length;
    prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_crypto_array = crypto_array;
    prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_crypto_array_size = crypto_array_size;
    prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_cipher_map = cipher_map;
    prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_cipher_map_size = cipher_map_size;
    prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_metadata_ptr = metadata_memory;
    prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_metadata_size = memory_size;
    prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_trusted_certificate = trusted_certificate;
    resource_ptr -> resource_mqtt_client_id_length = prov_client_ptr -> nx_azure_iot_provisioning_client_registration_id_length;
    resource_ptr -> resource_mqtt_client_id = prov_client_ptr -> nx_azure_iot_provisioning_client_registration_id;

    if (az_failed(az_iot_provisioning_client_init(&(prov_client_ptr -> nx_azure_iot_provisioning_client_core),
                                                  endpoint_span, id_scope_span, registration_id_span, NULL)))
    {
         LogError("IoTProvisioning client create fail: failed to intialize core client");
        return(NX_AZURE_IOT_SDK_CORE_ERROR);
    }

    status = _nxd_mqtt_client_cloud_create(mqtt_client_ptr, (CHAR *)nx_azure_iot_ptr -> nx_azure_iot_name,
                                           (CHAR *)resource_ptr -> resource_mqtt_client_id,
                                           resource_ptr -> resource_mqtt_client_id_length,
                                           nx_azure_iot_ptr -> nx_azure_iot_ip_ptr,
                                           nx_azure_iot_ptr -> nx_azure_iot_pool_ptr,
                                           &nx_azure_iot_ptr -> nx_azure_iot_cloud);
    if (status)
    {

        /* Release the mutex.  */
        tx_mutex_put(nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);
        LogError("IoTProvisioning client create fail: MQTT CLIENT CREATE FAIL: 0x%02x", status);
        return(status);
    }

    status = nxd_mqtt_client_receive_notify_set(mqtt_client_ptr,
                                                nx_azure_iot_provisioning_client_mqtt_receive_callback);
    if (status)
    {

        /* Release the mutex.  */
        tx_mutex_put(nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);
        LogError("IoTProvisioning client set message callback: 0x%02x", status);
        nxd_mqtt_client_delete(mqtt_client_ptr);
        return(status);
    }

     status = nx_azure_iot_buffer_allocate(prov_client_ptr -> nx_azure_iot_ptr,
                                        &buffer_ptr, &buffer_size, &buffer_context);
    if (status)
    {

        /* Release the mutex.  */
        tx_mutex_put(nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);
        LogError("IoTProvisioning client failed initialization: BUFFER ALLOCATE FAIL");
        nxd_mqtt_client_delete(mqtt_client_ptr);
        return(status);
    }

    /* Build user name.  */
    if (az_failed(az_iot_provisioning_client_get_user_name(&(prov_client_ptr -> nx_azure_iot_provisioning_client_core),
                                                           (CHAR *)buffer_ptr, buffer_size, &mqtt_user_name_length)))
    {
        LogError("IoTProvisioning client connect fail: NX_AZURE_IOT_Provisioning_CLIENT_USERNAME_SIZE is too small.");
        nx_azure_iot_buffer_free(buffer_context);
        nxd_mqtt_client_delete(mqtt_client_ptr);
        return(NX_AZURE_IOT_INSUFFICIENT_BUFFER_SPACE);
    }

    /* Save the resource buffer.  */
    resource_ptr -> resource_mqtt_buffer_context = buffer_context;
    resource_ptr -> resource_mqtt_buffer_size = buffer_size;
    resource_ptr -> resource_mqtt_user_name_length = mqtt_user_name_length;
    resource_ptr -> resource_mqtt_user_name = buffer_ptr;
    resource_ptr -> resource_mqtt_sas_token = buffer_ptr + mqtt_user_name_length;
    prov_client_ptr -> nx_azure_iot_provisioning_client_sas_token_buff_size = buffer_size - mqtt_user_name_length;

    /* Link the resource. */
    prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_data_ptr = (VOID *)prov_client_ptr;
    prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_type = NX_AZURE_IOT_RESOURCE_IOT_PROVISIONING;
    nx_azure_iot_resource_add(nx_azure_iot_ptr, &(prov_client_ptr -> nx_azure_iot_provisioning_client_resource));

    /* Set event processing routine.   */
    nx_azure_iot_ptr -> nx_azure_iot_provisioning_client_event_process = nx_azure_iot_provisioning_client_event_process;

    /* Update state.  */
    prov_client_ptr -> nx_azure_iot_provisioning_client_state = NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_INIT;

    /* Release the mutex.  */
    tx_mutex_put(nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);

    return(NX_AZURE_IOT_SUCCESS);
}

UINT nx_azure_iot_provisioning_client_deinitialize(NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr)
{
NX_AZURE_IOT_PROVISIONING_THREAD *thread_list_ptr;
UINT status;

    /* Check for invalid input pointers.  */
    if ((prov_client_ptr == NX_NULL) || (prov_client_ptr -> nx_azure_iot_ptr == NX_NULL))
    {
        LogError("IoTProvisioning client deinitialize fail: INVALID POINTER");
        return(NX_AZURE_IOT_INVALID_PARAMETER);
    }

    /* Obtain the mutex.  */
    tx_mutex_get(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr, NX_WAIT_FOREVER);

    if ((prov_client_ptr -> nx_azure_iot_provisioning_client_state > NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_INIT &&
         prov_client_ptr -> nx_azure_iot_provisioning_client_state < NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_DONE))
    {

        /* wake up all the threads */
        for (thread_list_ptr = prov_client_ptr -> nx_azure_iot_provisioning_client_thread_suspended;
             thread_list_ptr;
             thread_list_ptr = thread_list_ptr -> thread_next)
        {
            tx_thread_wait_abort(thread_list_ptr -> thread_ptr);
        }

        /* Delete the list */
        prov_client_ptr -> nx_azure_iot_provisioning_client_thread_suspended = NULL;
    }

    /* force to error state */
    prov_client_ptr -> nx_azure_iot_provisioning_client_state = NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_ERROR;
    prov_client_ptr -> nx_azure_iot_provisioning_client_on_complete_callback = NULL;

    if (prov_client_ptr -> nx_azure_iot_provisioning_client_last_response)
    {
        nx_packet_release(prov_client_ptr -> nx_azure_iot_provisioning_client_last_response);
        prov_client_ptr -> nx_azure_iot_provisioning_client_last_response = NULL;
    }

    if (prov_client_ptr -> nx_azure_iot_provisioning_client_response.packet_ptr)
    {
        nx_packet_release(prov_client_ptr -> nx_azure_iot_provisioning_client_response.packet_ptr);
        prov_client_ptr -> nx_azure_iot_provisioning_client_response.packet_ptr = NULL;
    }

    /* Release the mqtt connection resource.  */
    if (prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_mqtt_buffer_context)
    {
        nx_azure_iot_buffer_free(prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_mqtt_buffer_context);
        prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_mqtt_buffer_context = NX_NULL;
    }

    /* Release the mutex.  */
    tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);

    /* Disconnect */
    nxd_mqtt_client_disconnect(&prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_mqtt);

    /* Delete the client */
    nxd_mqtt_client_delete(&prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_mqtt);

    /* Obtain the mutex.  */
    tx_mutex_get(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr, NX_WAIT_FOREVER);

    /* Remove resource from list.  */
    status = nx_azure_iot_resource_remove(prov_client_ptr -> nx_azure_iot_ptr, &(prov_client_ptr -> nx_azure_iot_provisioning_client_resource));

    if (status)
    {

        /* Release the mutex.  */
        tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);
        LogError("IoTProvisioning client handle not found");
        return(status);
    }

    /* Release the mutex.  */
    tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);

    return(NX_AZURE_IOT_SUCCESS);
}

UINT nx_azure_iot_provisioning_client_device_cert_set(NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr,
                                                      NX_SECURE_X509_CERT *x509_cert)
{
    if ((prov_client_ptr == NX_NULL) || (prov_client_ptr -> nx_azure_iot_ptr == NX_NULL))
    {
        LogError("IoTProvisioning device cert set fail: INVALID POINTER");
        return(NX_AZURE_IOT_INVALID_PARAMETER);
    }

    /* Obtain the mutex.  */
    tx_mutex_get(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr, NX_WAIT_FOREVER);

    prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_device_certificate = x509_cert;

    /* Release the mutex.  */
    tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);

    return(NX_AZURE_IOT_SUCCESS);
}


static VOID nx_azure_iot_provisioning_client_event_process(NX_AZURE_IOT *nx_azure_iot_ptr,
                                                           ULONG common_events, ULONG module_own_events)
{
NX_AZURE_IOT_RESOURCE *resource;
NX_AZURE_IOT_PROVISIONING_CLIENT *provisioning_client;

    /* Process module own events.  */
    LogDebug("Event generated common event: 0x%lx, module event: 0x%lx \r\n", common_events, module_own_events);

    /* Obtain the mutex.  */
    tx_mutex_get(nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr, NX_WAIT_FOREVER);

    /* Loop to check IoT Provisioning Client.  */
    for (resource = nx_azure_iot_ptr -> nx_azure_iot_resource_list_header; resource;
         resource = resource -> resource_next)
    {
        if (resource -> resource_type != NX_AZURE_IOT_RESOURCE_IOT_PROVISIONING)
        {
            continue;
        }

        /* Set provisioning client pointer.  */
        provisioning_client = (NX_AZURE_IOT_PROVISIONING_CLIENT *)resource -> resource_data_ptr;

        if (common_events & NX_CLOUD_COMMON_PERIODIC_EVENT)
        {
            nx_azure_iot_provisioning_client_process_timer(provisioning_client);
        }

        if (module_own_events & NX_AZURE_IOT_PROVISIONING_CLIENT_CONNECT_EVENT)
        {
            nx_azure_iot_provisioning_client_process_connect(provisioning_client);
        }

        if (module_own_events & NX_AZURE_IOT_PROVISIONING_CLIENT_SUBSCRIBE_EVENT)
        {
            nx_azure_iot_provisioning_client_subscribe(provisioning_client);
        }

        if (module_own_events & NX_AZURE_IOT_PROVISIONING_CLIENT_RESPONSE_EVENT)
        {
            nx_azure_iot_provisioning_client_process_service_response(provisioning_client);
        }

        if (module_own_events & NX_AZURE_IOT_PROVISIONING_CLIENT_REQUEST_EVENT)
        {
            nx_azure_iot_provisioning_client_generate_service_request(provisioning_client);
        }

        if (module_own_events & NX_AZURE_IOT_PROVISIONING_CLIENT_DISCONNECT_EVENT)
        {
            nx_azure_iot_provisioning_client_process_disconnect(provisioning_client);
        }
    }

    /* Release the mutex.  */
    tx_mutex_put(nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);
}

UINT nx_azure_iot_provisioning_client_register(NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr, UINT wait_option)
{
NX_AZURE_IOT_PROVISIONING_THREAD thread_list;
UINT old_threshold;
UINT status;

    if ((prov_client_ptr == NX_NULL) || (prov_client_ptr -> nx_azure_iot_ptr == NX_NULL))
    {
        LogError("IoTProvisioning register fail: INVALID POINTER");
        return(NX_AZURE_IOT_INVALID_PARAMETER);
    }

    if (prov_client_ptr -> nx_azure_iot_provisioning_client_state == NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_NONE)
    {
        LogError("IoTProvisioning register fail: not intialized");
        return(NX_AZURE_IOT_NOT_INITIALIZED);
    }

    /* Set callback function for disconnection. */
    nxd_mqtt_client_disconnect_notify_set(&(prov_client_ptr -> nx_azure_iot_provisioning_client_resource.resource_mqtt),
                                          nx_azure_iot_provisioning_client_mqtt_disconnect_notify);

    /* Obtain the mutex.  */
    tx_mutex_get(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr, NX_WAIT_FOREVER);

    if (prov_client_ptr -> nx_azure_iot_provisioning_client_state == NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_INIT)
    {

        /* Update state in user thread under mutex */
        nx_azure_iot_provisioning_client_update_state(prov_client_ptr, NX_AZURE_IOT_PENDING);

        /* Trigger workflow */
        status = nx_cloud_module_event_set(&(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_cloud_module),
                                           NX_AZURE_IOT_PROVISIONING_CLIENT_CONNECT_EVENT);
        if (status)
        {
            nx_azure_iot_provisioning_client_update_state(prov_client_ptr, status);
        }
    }

    if (wait_option)
    {
        if (prov_client_ptr -> nx_azure_iot_provisioning_client_state > NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_INIT &&
             prov_client_ptr -> nx_azure_iot_provisioning_client_state < NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_DONE)
        {
            thread_list.thread_next = prov_client_ptr -> nx_azure_iot_provisioning_client_thread_suspended;
            thread_list.thread_ptr = tx_thread_identify();
            prov_client_ptr -> nx_azure_iot_provisioning_client_thread_suspended = &thread_list;

            /* Disable preemption. */
            tx_thread_preemption_change(tx_thread_identify(), 0, &old_threshold);

            /* Release the mutex.  */
            tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);

            tx_thread_sleep(wait_option);

            /* Obtain the mutex.  */
            tx_mutex_get(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr, NX_WAIT_FOREVER);

            /* Restore preemption. */
            tx_thread_preemption_change(tx_thread_identify(), old_threshold, &old_threshold);

            nx_azure_iot_provisioning_client_thread_dequeue(prov_client_ptr, &thread_list);
        }
    }

    if (prov_client_ptr -> nx_azure_iot_provisioning_client_state > NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_INIT &&
         prov_client_ptr -> nx_azure_iot_provisioning_client_state < NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_DONE)
    {

        /* Release the mutex.  */
        tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);
        return(NX_AZURE_IOT_PENDING);
    }
    else if (prov_client_ptr -> nx_azure_iot_provisioning_client_state == NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_ERROR)
    {

        /* Release the mutex.  */
        tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);
        LogError("IoTProvisioning register fail: Error out");
        return(prov_client_ptr -> nx_azure_iot_provisioning_client_result);
    }

    /* Release the mutex.  */
    tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);

    return(NX_AZURE_IOT_SUCCESS);
}

UINT nx_azure_iot_provisioning_client_completion_callback_set(NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr,
                                                              VOID (*on_complete_callback)(
                                                                  struct NX_AZURE_IOT_PROVISIONING_CLIENT_STRUCT *prov_client_ptr, UINT status))
{
    if ((prov_client_ptr == NX_NULL) || (prov_client_ptr -> nx_azure_iot_ptr == NX_NULL))
    {
        LogError("IoTProvisioning register fail: INVALID POINTER");
        return(NX_AZURE_IOT_INVALID_PARAMETER);
    }

    /* Obtain the mutex.  */
    tx_mutex_get(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr, NX_WAIT_FOREVER);

    prov_client_ptr -> nx_azure_iot_provisioning_client_on_complete_callback = on_complete_callback;

    /* Release the mutex.  */
    tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);

    return(NX_AZURE_IOT_SUCCESS);
}

UINT nx_azure_iot_provisioning_client_symmetric_key_set(NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr,
                                                        UCHAR *symmetric_key, UINT symmetric_key_length)
{
ULONG expiry_time_secs;
UINT status;

    if ((prov_client_ptr == NX_NULL) || (prov_client_ptr -> nx_azure_iot_ptr == NX_NULL) ||
        (symmetric_key == NX_NULL) || (symmetric_key_length == 0))
    {
        LogError("IoTProvisioning client symmetric key fail: Invalid argument");
        return(NX_AZURE_IOT_INVALID_PARAMETER);
    }

    /* Obtain the mutex.  */
    tx_mutex_get(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr, NX_WAIT_FOREVER);

    prov_client_ptr -> nx_azure_iot_provisioning_client_symmetric_key = symmetric_key;
    prov_client_ptr -> nx_azure_iot_provisioning_client_symmetric_key_length = symmetric_key_length;

    status = nx_azure_iot_unix_time_get(prov_client_ptr -> nx_azure_iot_ptr, &expiry_time_secs);
    if (status)
    {

        /* Release the mutex.  */
        tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);
        LogError("IoTProvisioning client symmetric key fail: 0x%02x", status);
        return(status);
    }

    expiry_time_secs += NX_AZURE_IOT_PROVISIONING_CLIENT_TOKEN_EXPIRY;

    status = nx_azure_iot_provisioning_client_sas_token_get(prov_client_ptr, expiry_time_secs);
    if (status)
    {

        /* Release the mutex.  */
        tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);
        LogError("IoTProvisioning client symmetric key fail: sas token generation failed");
        return(status);
    }

    /* Release the mutex.  */
    tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);

    return(NX_AZURE_IOT_SUCCESS);
}

UINT nx_azure_iot_provisioning_client_iothub_device_info_get(NX_AZURE_IOT_PROVISIONING_CLIENT *prov_client_ptr,
                                                             UCHAR *iothub_hostname, UINT *iothub_hostname_len,
                                                             UCHAR *device_id, UINT *device_id_len)
{
UINT status;
az_span *device_id_span_ptr;
az_span *assigned_hub_span_ptr;

    if ((prov_client_ptr == NX_NULL) || (prov_client_ptr -> nx_azure_iot_ptr == NX_NULL) ||
        (iothub_hostname == NX_NULL) || (iothub_hostname_len == NX_NULL) ||
        (device_id == NX_NULL) || (device_id_len == NX_NULL))
    {
        LogError("IoTProvisioning client iothub device info get fail: Invalid argument");
        return(NX_AZURE_IOT_INVALID_PARAMETER);
    }

    /* Obtain the mutex.  */
    status = tx_mutex_get(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr, NX_WAIT_FOREVER);
    if (status)
    {
        LogError("IoTProvisioning client iothub get fail: get mutex");
        return(status);
    }

    if (prov_client_ptr -> nx_azure_iot_provisioning_client_state != NX_AZURE_IOT_PROVISIONING_CLIENT_STATUS_DONE)
    {
        LogError("IoTProvisioning client iothub device info get fail: wrong state");
        tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);
        return(NX_AZURE_IOT_WRONG_STATE);
    }

    device_id_span_ptr = &(prov_client_ptr -> nx_azure_iot_provisioning_client_response.register_response.registration_result.device_id);
    assigned_hub_span_ptr = &(prov_client_ptr -> nx_azure_iot_provisioning_client_response.register_response.registration_result.assigned_hub_hostname);
    if ((UINT)az_span_size(*assigned_hub_span_ptr) >= *iothub_hostname_len || (UINT)az_span_size(*device_id_span_ptr) > *device_id_len)
    {
        LogError("IoTProvisioning client iothub device info get fail: insufficient memory");
        tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);
        return(NX_AZURE_IOT_INSUFFICIENT_BUFFER_SPACE);
    }

    /* iothub hostname should be null terminated */
    memcpy((VOID *)iothub_hostname, (VOID *)az_span_ptr(*assigned_hub_span_ptr), (UINT)az_span_size(*assigned_hub_span_ptr));
    iothub_hostname[az_span_size(*assigned_hub_span_ptr)] = 0;
    *iothub_hostname_len = (UINT)az_span_size(*assigned_hub_span_ptr);

    memcpy((VOID *)device_id, (VOID *)az_span_ptr(*device_id_span_ptr), (UINT)az_span_size(*device_id_span_ptr));
    *device_id_len = (UINT)az_span_size(*device_id_span_ptr);

    tx_mutex_put(prov_client_ptr -> nx_azure_iot_ptr -> nx_azure_iot_mutex_ptr);

    return(NX_AZURE_IOT_SUCCESS);
}
