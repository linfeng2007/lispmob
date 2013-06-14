/*
 * lispd_info_reply.c
 *
 * This file is part of LISP Mobile Node Implementation.
 * Receive and process Info-Reply messages
 *
 *
 * Copyright (C) 2011 Cisco Systems, Inc, 2011. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Please send any bug reports or fixes you make to the email address(es):
 *    LISP-MN developers <devel@lispmob.org>
 *
 * Written or modified by:
 *    Alberto Rodriguez Natal    <arnatal@ac.upc.edu>
 *
 */


#include <sys/timerfd.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include "lispd_afi.h"
#include "lispd_external.h"
#include "lispd_info_reply.h"
#include "lispd_lib.h"
#include "lispd_local_db.h"
#include "lispd_map_register.h"
#include "cksum.h"




/*
 *  Process an Info-Reply Message
 *  Receive an Info-Reply message and process based on control bits
 *
 */

int process_info_reply_msg(
        uint8_t         *packet,
        lisp_addr_t     local_rloc)
{

    uint8_t                     *ptr                    = packet;

    uint8_t                     lisp_type               = 0;
    uint8_t                     reply                   = 0;

    uint64_t                    nonce                   = 0;
    uint16_t                    key_id                  = 0;
    uint16_t                    auth_data_len           = 0;
	uint8_t                     *auth_data_pos          = NULL;

    uint32_t                    ttl                     = 0;
    uint8_t                     eid_mask_len            = 0;
    lisp_addr_t                 eid_prefix              = {.afi=AF_UNSPEC};

    uint16_t                    ms_udp_port             = 0;
    uint16_t                    etr_udp_port            = 0;

    uint32_t                    info_reply_hdr_len      = 0;
    uint32_t                    lcaf_addr_len           = 0;
	uint32_t                    pckt_len                = 0;

	uint16_t                    *lcaf_afi               = NULL;



    lisp_addr_t                 global_etr_rloc         = {.afi=AF_UNSPEC};
    lisp_addr_t                 ms_rloc                 = {.afi=AF_UNSPEC};
    lisp_addr_t                 private_etr_rloc        = {.afi=AF_UNSPEC};
    lispd_rtr_locators_list     *rtr_locators_list      = NULL;

    lispd_mapping_elt           *mapping                = NULL;
    lispd_locator_elt           *locator                = NULL;
    lcl_locator_extended_info   *lcl_locator_ext_inf    = NULL;


    char                        rtrs_list_str[2000];
    lispd_rtr_locators_list     *aux_rtr_locators_list  = NULL;

    uint8_t                     is_behind_nat           = FALSE;



    /*
     * Get source port and address.
     */


    err = extract_info_nat_header(ptr,
            &lisp_type,
            &reply,
            &nonce,
            &key_id,
            &auth_data_len,
            &auth_data_pos,
            &ttl,
            &eid_mask_len,
            &eid_prefix,
            &info_reply_hdr_len);

    if (err != GOOD){
        lispd_log_msg(LISP_LOG_DEBUG_1,"process_info_reply_msg: Couldn't process Info Reply message");
        return (BAD);
    }
    ptr = CO(ptr,info_reply_hdr_len);

    lcaf_afi = (uint16_t *)ptr;
    if ( ntohs(*lcaf_afi) != LISP_AFI_LCAF){
        lispd_log_msg(LISP_LOG_DEBUG_1,"process_info_reply_msg: Malformed packet");
        return (BAD);
    }

    ptr = CO(ptr,FIELD_AFI_LEN);
	/* Extract Info-Reply body fields */
    err = extract_nat_lcaf_data(ptr,
            &ms_udp_port,
            &etr_udp_port,
            &global_etr_rloc,
            &ms_rloc,
            &private_etr_rloc,
            &rtr_locators_list,
            &lcaf_addr_len);

    /* Leave only RTR with same afi as the local rloc where we received the message */
    remove_rtr_locators_with_afi_different_to(&rtr_locators_list, local_rloc.afi);

    lcaf_addr_len += FIELD_AFI_LEN;

    if (err == BAD) {
        lispd_log_msg(LISP_LOG_DEBUG_2, "process_info_reply_msg: Error extracting packet data");
        return (BAD);
    }

    /* Print the extracted information of the message */
    if (is_loggable(LISP_LOG_DEBUG_2)){
        aux_rtr_locators_list = rtr_locators_list;
        if (aux_rtr_locators_list != NULL){
            sprintf(rtrs_list_str, "  %s ", get_char_from_lisp_addr_t(aux_rtr_locators_list->locator->address));
            aux_rtr_locators_list = aux_rtr_locators_list->next;
        }
        while (aux_rtr_locators_list != NULL){
            sprintf(rtrs_list_str + strlen(rtrs_list_str), "  %s ", get_char_from_lisp_addr_t(aux_rtr_locators_list->locator->address));
            aux_rtr_locators_list = aux_rtr_locators_list->next;
        }
        lispd_log_msg(LISP_LOG_DEBUG_2, "Info-Reply message data->"
                "Nonce: %s , KeyID: %hu ,TTL: %u , EID-prefix: %s/%hhu , "
                "MS UDP Port Number: %hu , ETR UDP Port Number: %hu , Global ETR RLOC Address: %s , "
                "MS RLOC Address: %s , Private ETR RLOC Address: %s, RTR RLOC list: %s",
                get_char_from_nonce(nonce), key_id, ttl, get_char_from_lisp_addr_t(eid_prefix),eid_mask_len,
                ms_udp_port, etr_udp_port, get_char_from_lisp_addr_t(global_etr_rloc),
                get_char_from_lisp_addr_t(ms_rloc),get_char_from_lisp_addr_t(private_etr_rloc),rtrs_list_str);
    }


    pckt_len = info_reply_hdr_len + lcaf_addr_len;

    if(BAD == check_auth_field(key_id,
                                 map_servers->key,
                                 (void *) packet,
                                 pckt_len,
                                 auth_data_pos)){
        lispd_log_msg(LISP_LOG_DEBUG_2, "Info-Reply: Error checking auth data field");
        return(BAD);
    }else{
        lispd_log_msg(LISP_LOG_DEBUG_2, "Info-Reply: Correct auth data field checking");
    }

	
	// TODO  Select the best RTR from the list retrieved from the Info-Reply


    /* Check if behind NAT */

    switch (compare_lisp_addr_t(&global_etr_rloc, &local_rloc)) {
    case 0:
        is_behind_nat = FALSE;
        lispd_log_msg(LISP_LOG_DEBUG_2, "NAT Traversal: MN is not behind NAT");
        break;
    case 1:
    case 2:
        is_behind_nat = TRUE;
        lispd_log_msg(LISP_LOG_DEBUG_2, "NAT Traversal: MN is behind NAT");
        break;
    case -1:
        is_behind_nat = UNKNOWN;
        lispd_log_msg(LISP_LOG_DEBUG_2, "NAT Traversal: Unknown state");
        break;
    }

    if (is_behind_nat == TRUE){

        mapping = lookup_eid_exact_in_db(eid_prefix, eid_mask_len);
        if (mapping == NULL){
            lispd_log_msg(LISP_LOG_DEBUG_2, "process_info_reply_msg: Info Reply is not for any local EID");
            return (BAD);
        }
        locator = get_locator_from_mapping(mapping,local_rloc);
        if (locator == NULL){
            lispd_log_msg(LISP_LOG_DEBUG_2, "process_info_reply_msg: Info Reply received in the wrong locator");
            return (BAD);
        }

        lcl_locator_ext_inf = (lcl_locator_extended_info *)locator->extended_info;
        if (lcl_locator_ext_inf->rtr_locators_list != NULL){
            free_rtr_list(lcl_locator_ext_inf->rtr_locators_list);
        }
        lcl_locator_ext_inf->rtr_locators_list = rtr_locators_list;

        if (nat_status == FULL_NAT || nat_status == PARTIAL_NAT){
            nat_status = PARTIAL_NAT;
        }else{
            nat_status = FULL_NAT;
        }
    }else{
        if (nat_status == FULL_NAT || nat_status == PARTIAL_NAT){
            nat_status = PARTIAL_NAT;
        }else{
            nat_status = NO_NAT;
        }
    }

    /* Once we know the NAT state we send a Map-Register */

    map_register(NULL,NULL);

    return (GOOD);
}


/*
 * Editor modelines
 *
 * vi: set shiftwidth=4 tabstop=4 expandtab:
 * :indentSize=4:tabSize=4:noTabs=true:
 */