/******************************************************************************
 *
 *  Copyright (C) 2000-2012 Broadcom Corporation
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

/*****************************************************************************
 *
 *  This file contains functions that manages ACL link modes.
 *  This includes operations such as active, hold,
 *  park and sniff modes.
 *
 *  This module contains both internal and external (API)
 *  functions. External (API) functions are distinguishable
 *  by their names beginning with uppercase BTM.
 *
 *****************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include "bt_types.h"
#include "gki.h"
#include "hcimsgs.h"
#include "btu.h"
#include "btm_api.h"
#include "btm_int.h"
#include "l2c_int.h"
#include "hcidefs.h"


#if BTM_PWR_MGR_INCLUDED == TRUE

/* This compile option is only useful when the FW has a bug
 * it automatically uses single slot when entering SNIFF mode, but does not restore the setting
 * This issue was found when A2DP link goes into sniff and existing sniff still has choppy audio.
 * If this issue is seen, look for FW solution first.
 * This work around code will be removed later. */
#ifndef BTM_PM_SNIFF_SLOT_WORK_AROUND
#define BTM_PM_SNIFF_SLOT_WORK_AROUND       FALSE
#endif

/*****************************************************************************/
/*      to handle different modes                                            */
/*****************************************************************************/
#define BTM_PM_STORED_MASK      0x80 /* set this mask if the command is stored */
#define BTM_PM_NUM_SET_MODES    3 /* only hold, sniff & park */

/* Usage:  (ptr_features[ offset ] & mask )?TRUE:FALSE */
/* offset to supported feature */
const UINT8 btm_pm_mode_off[BTM_PM_NUM_SET_MODES] = {0,    0,    1};
/* mask to supported feature */
const UINT8 btm_pm_mode_msk[BTM_PM_NUM_SET_MODES] = {0x40, 0x80, 0x01};

#define BTM_PM_GET_MD1      1
#define BTM_PM_GET_MD2      2
#define BTM_PM_GET_COMP     3

const UINT8 btm_pm_md_comp_matrix[BTM_PM_NUM_SET_MODES*BTM_PM_NUM_SET_MODES] =
{
    BTM_PM_GET_COMP,
    BTM_PM_GET_MD2,
    BTM_PM_GET_MD2,

    BTM_PM_GET_MD1,
    BTM_PM_GET_COMP,
    BTM_PM_GET_MD1,

    BTM_PM_GET_MD1,
    BTM_PM_GET_MD2,
    BTM_PM_GET_COMP
};

/* function prototype */
static int btm_pm_find_acl_ind(BD_ADDR remote_bda);
static tBTM_STATUS btm_pm_snd_md_req( UINT8 pm_id, int link_ind, tBTM_PM_PWR_MD *p_mode );

/*
#ifdef BTM_PM_DEBUG
#undef BTM_PM_DEBUG
#define BTM_PM_DEBUG    TRUE
#endif
*/

#if BTM_PM_DEBUG == TRUE
const char * btm_pm_state_str[] =
{
    "pm_active_state",
    "pm_hold_state",
    "pm_sniff_state",
    "pm_park_state",
    "pm_pend_state"
};

const char * btm_pm_event_str[] =
{
    "pm_set_mode_event",
    "pm_hci_sts_event",
    "pm_mod_chg_event",
    "pm_update_event"
};

const char * btm_pm_action_str[] =
{
    "pm_set_mode_action",
    "pm_update_db_action",
    "pm_mod_chg_action",
    "pm_hci_sts_action",
    "pm_update_action"
};
#endif

/*****************************************************************************/
/*                     P U B L I C  F U N C T I O N S                        */
/*****************************************************************************/
/*******************************************************************************
**
** Function         BTM_PmRegister
**
** Description      register or deregister with power manager
**
** Returns          BTM_SUCCESS if successful,
**                  BTM_NO_RESOURCES if no room to hold registration
**                  BTM_ILLEGAL_VALUE
**
*******************************************************************************/
tBTM_STATUS BTM_PmRegister (UINT8 mask, UINT8 *p_pm_id, tBTM_PM_STATUS_CBACK *p_cb)
{
    int xx;

    /* de-register */
    if(mask & BTM_PM_DEREG)
    {
        if(*p_pm_id >= BTM_MAX_PM_RECORDS)
            return BTM_ILLEGAL_VALUE;
        btm_cb.pm_reg_db[*p_pm_id].mask = BTM_PM_REC_NOT_USED;
        return BTM_SUCCESS;
    }

    for(xx=0; xx<BTM_MAX_PM_RECORDS; xx++)
    {
        /* find an unused entry */
        if(btm_cb.pm_reg_db[xx].mask == BTM_PM_REC_NOT_USED)
        {
            /* if register for notification, should provide callback routine */
            if(mask & BTM_PM_REG_NOTIF)
            {
                if(p_cb == NULL)
                    return BTM_ILLEGAL_VALUE;
                btm_cb.pm_reg_db[xx].cback = p_cb;
            }
            btm_cb.pm_reg_db[xx].mask = mask;
            *p_pm_id = xx;
            return BTM_SUCCESS;
        }
    }

    return BTM_NO_RESOURCES;
}

/*******************************************************************************
**
** Function         BTM_SetPowerMode
**
** Description      store the mode in control block or
**                  alter ACL connection behavior.
**
** Returns          BTM_SUCCESS if successful,
**                  BTM_UNKNOWN_ADDR if bd addr is not active or bad
**
*******************************************************************************/
tBTM_STATUS BTM_SetPowerMode (UINT8 pm_id, BD_ADDR remote_bda, tBTM_PM_PWR_MD *p_mode)
{
    UINT8               *p_features;
    int               ind, acl_ind;
    tBTM_PM_MCB *p_cb = NULL;   /* per ACL link */
    tBTM_PM_MODE        mode;
    int                 temp_pm_id;


    if(pm_id >= BTM_MAX_PM_RECORDS)
        pm_id = BTM_PM_SET_ONLY_ID;

    if(p_mode == NULL)
        return BTM_ILLEGAL_VALUE;

    BTM_TRACE_API3( "BTM_SetPowerMode: pm_id %d BDA: %08x mode:0x%x", pm_id,
                   (remote_bda[2]<<24)+(remote_bda[3]<<16)+(remote_bda[4]<<8)+remote_bda[5], p_mode->mode);

    /* take out the force bit */
    mode = p_mode->mode & ~BTM_PM_MD_FORCE;

    acl_ind = btm_pm_find_acl_ind(remote_bda);
    if(acl_ind == MAX_L2CAP_LINKS)
        return (BTM_UNKNOWN_ADDR);

    p_cb = &(btm_cb.pm_mode_db[acl_ind]);

    if(mode != BTM_PM_MD_ACTIVE)
    {
        /* check if the requested mode is supported */
        ind = mode - BTM_PM_MD_HOLD; /* make it base 0 */
        p_features = BTM_ReadLocalFeatures();
        if( !(p_features[ btm_pm_mode_off[ind] ] & btm_pm_mode_msk[ind] ) )
            return BTM_MODE_UNSUPPORTED;
    }

    if(mode == p_cb->state) /* the requested mode is current mode */
    {
        /* already in the requested mode and the current interval has less latency than the max */
        if( (mode == BTM_PM_MD_ACTIVE) ||
            ((p_mode->mode & BTM_PM_MD_FORCE) && (p_mode->max >= p_cb->interval) && (p_mode->min <= p_cb->interval)) ||
            ((p_mode->mode & BTM_PM_MD_FORCE)==0 && (p_mode->max >= p_cb->interval)) )
        {
            BTM_TRACE_DEBUG4( "BTM_SetPowerMode: mode:0x%x interval %d max:%d, min:%d", p_mode->mode, p_cb->interval, p_mode->max, p_mode->min);
            return BTM_SUCCESS;
        }
    }

    temp_pm_id = pm_id;
    if(pm_id == BTM_PM_SET_ONLY_ID)
        temp_pm_id = BTM_MAX_PM_RECORDS;

    /* update mode database */
    if( ((pm_id != BTM_PM_SET_ONLY_ID) &&
         (btm_cb.pm_reg_db[pm_id].mask & BTM_PM_REG_SET))
       || ((pm_id == BTM_PM_SET_ONLY_ID) && (btm_cb.pm_pend_link != MAX_L2CAP_LINKS)) )
    {
#if BTM_PM_DEBUG == TRUE
    BTM_TRACE_DEBUG2( "BTM_SetPowerMode: Saving cmd acl_ind %d temp_pm_id %d", acl_ind,temp_pm_id);
#endif
        /* Make sure mask is set to BTM_PM_REG_SET */
        btm_cb.pm_reg_db[temp_pm_id].mask |= BTM_PM_REG_SET;
        *(&p_cb->req_mode[temp_pm_id]) = *((tBTM_PM_PWR_MD *)p_mode);
        p_cb->chg_ind = TRUE;
    }

#if BTM_PM_DEBUG == TRUE
    BTM_TRACE_DEBUG2( "btm_pm state:0x%x, pm_pend_link: %d", p_cb->state, btm_cb.pm_pend_link);
#endif
    /* if mode == hold or pending, return */
    if( (p_cb->state == BTM_PM_STS_HOLD) ||
        (p_cb->state ==  BTM_PM_STS_PENDING) ||
        (btm_cb.pm_pend_link != MAX_L2CAP_LINKS) ) /* command pending */
    {
        if(acl_ind != btm_cb.pm_pend_link)
        {
            /* set the stored mask */
            p_cb->state |= BTM_PM_STORED_MASK;
            BTM_TRACE_DEBUG1( "btm_pm state stored:%d",acl_ind);
        }
        return BTM_CMD_STORED;
    }



    return btm_pm_snd_md_req(pm_id, acl_ind, p_mode);
}

/*******************************************************************************
**
** Function         BTM_ReadPowerMode
**
** Description      This returns the current mode for a specific
**                  ACL connection.
**
** Input Param      remote_bda - device address of desired ACL connection
**
** Output Param     p_mode - address where the current mode is copied into.
**                          BTM_ACL_MODE_NORMAL
**                          BTM_ACL_MODE_HOLD
**                          BTM_ACL_MODE_SNIFF
**                          BTM_ACL_MODE_PARK
**                          (valid only if return code is BTM_SUCCESS)
**
** Returns          BTM_SUCCESS if successful,
**                  BTM_UNKNOWN_ADDR if bd addr is not active or bad
**
*******************************************************************************/
tBTM_STATUS BTM_ReadPowerMode (BD_ADDR remote_bda, tBTM_PM_MODE *p_mode)
{
    int acl_ind;

    if( (acl_ind = btm_pm_find_acl_ind(remote_bda)) == MAX_L2CAP_LINKS)
        return (BTM_UNKNOWN_ADDR);

    *p_mode = btm_cb.pm_mode_db[acl_ind].state;
    return BTM_SUCCESS;
}

/*******************************************************************************
**
** Function         btm_read_power_mode_state
**
** Description      This returns the current pm state for a specific
**                  ACL connection.
**
** Input Param      remote_bda - device address of desired ACL connection
**
** Output Param     pmState - address where the current  pm state is copied into.
**                          BTM_PM_ST_ACTIVE
**                          BTM_PM_ST_HOLD
**                          BTM_PM_ST_SNIFF
**                          BTM_PM_ST_PARK
**                          BTM_PM_ST_PENDING
**                          (valid only if return code is BTM_SUCCESS)
**
** Returns          BTM_SUCCESS if successful,
**                  BTM_UNKNOWN_ADDR if bd addr is not active or bad
**
*******************************************************************************/
tBTM_STATUS btm_read_power_mode_state (BD_ADDR remote_bda, tBTM_PM_STATE *pmState)
{
    int acl_ind;

    if( (acl_ind = btm_pm_find_acl_ind(remote_bda)) == MAX_L2CAP_LINKS)
        return (BTM_UNKNOWN_ADDR);

    *pmState= btm_cb.pm_mode_db[acl_ind].state;
    return BTM_SUCCESS;
}


/*******************************************************************************
**
** Function         BTM_SetSsrParams
**
** Description      This sends the given SSR parameters for the given ACL
**                  connection if it is in ACTIVE mode.
**
** Input Param      remote_bda - device address of desired ACL connection
**                  max_lat    - maximum latency (in 0.625ms)(0-0xFFFE)
**                  min_rmt_to - minimum remote timeout
**                  min_loc_to - minimum local timeout
**
**
** Returns          BTM_SUCCESS if the HCI command is issued successful,
**                  BTM_UNKNOWN_ADDR if bd addr is not active or bad
**                  BTM_CMD_STORED if the command is stored
**
*******************************************************************************/
tBTM_STATUS BTM_SetSsrParams (BD_ADDR remote_bda, UINT16 max_lat,
                              UINT16 min_rmt_to, UINT16 min_loc_to)
{
#if (BTM_SSR_INCLUDED == TRUE)
    int acl_ind;
    tBTM_PM_MCB *p_cb;

    if( (acl_ind = btm_pm_find_acl_ind(remote_bda)) == MAX_L2CAP_LINKS)
        return (BTM_UNKNOWN_ADDR);

    if(BTM_PM_STS_ACTIVE == btm_cb.pm_mode_db[acl_ind].state ||
        BTM_PM_STS_SNIFF == btm_cb.pm_mode_db[acl_ind].state)
    {
        if (btsnd_hcic_sniff_sub_rate(btm_cb.acl_db[acl_ind].hci_handle, max_lat,
                                      min_rmt_to, min_loc_to))
            return BTM_SUCCESS;
        else
            return BTM_NO_RESOURCES;
    }
    p_cb = &btm_cb.pm_mode_db[acl_ind];
    p_cb->max_lat       = max_lat;
    p_cb->min_rmt_to    = min_rmt_to;
    p_cb->min_loc_to    = min_loc_to;
    return BTM_CMD_STORED;
#else
    return BTM_ILLEGAL_ACTION;
#endif
}

/*******************************************************************************
**
** Function         btm_pm_reset
**
** Description      as a part of the BTM reset process.
**
** Returns          void
**
*******************************************************************************/
void btm_pm_reset(void)
{
    int xx;
    tBTM_PM_STATUS_CBACK *cb = NULL;

    /* clear the pending request for application */
    if( (btm_cb.pm_pend_id != BTM_PM_SET_ONLY_ID) &&
        (btm_cb.pm_reg_db[btm_cb.pm_pend_id].mask & BTM_PM_REG_NOTIF) )
    {
        cb = btm_cb.pm_reg_db[btm_cb.pm_pend_id].cback;
    }

    /* no command pending */
    btm_cb.pm_pend_link = MAX_L2CAP_LINKS;

    /* clear the register record */
    for(xx=0; xx<BTM_MAX_PM_RECORDS; xx++)
    {
        btm_cb.pm_reg_db[xx].mask = BTM_PM_REC_NOT_USED;
    }

    if(cb != NULL)
        (*cb)(btm_cb.acl_db[btm_cb.pm_pend_link].remote_addr, BTM_PM_STS_ERROR, BTM_DEV_RESET, 0);
}

/*******************************************************************************
**
** Function         btm_pm_sm_alloc
**
** Description      This function initializes the control block of an ACL link.
**                  It is called when an ACL connection is created.
**
** Returns          void
**
*******************************************************************************/
void btm_pm_sm_alloc(UINT8 ind)
{
    tBTM_PM_MCB *p_db = &btm_cb.pm_mode_db[ind];   /* per ACL link */
    memset (p_db, 0, sizeof(tBTM_PM_MCB));
    p_db->state = BTM_PM_ST_ACTIVE;
#if BTM_PM_DEBUG == TRUE
    BTM_TRACE_DEBUG2( "btm_pm_sm_alloc ind:%d st:%d", ind, p_db->state);
#endif
}

/*******************************************************************************
**
** Function         btm_pm_find_acl_ind
**
** Description      This function initializes the control block of an ACL link.
**                  It is called when an ACL connection is created.
**
** Returns          void
**
*******************************************************************************/
static int btm_pm_find_acl_ind(BD_ADDR remote_bda)
{
    tACL_CONN   *p = &btm_cb.acl_db[0];
    UINT8 xx;

    for (xx = 0; xx < MAX_L2CAP_LINKS; xx++, p++)
    {
        if ((p->in_use) && (!memcmp (p->remote_addr, remote_bda, BD_ADDR_LEN)))
        {
#if BTM_PM_DEBUG == TRUE
            BTM_TRACE_DEBUG2( "btm_pm_find_acl_ind ind:%d, st:%d", xx, btm_cb.pm_mode_db[xx].state);
#endif
            break;
        }
    }
    return xx;
}

/*******************************************************************************
**
** Function     btm_pm_compare_modes
** Description  get the "more active" mode of the 2
** Returns      void
**
*******************************************************************************/
static tBTM_PM_PWR_MD * btm_pm_compare_modes(tBTM_PM_PWR_MD *p_md1, tBTM_PM_PWR_MD *p_md2, tBTM_PM_PWR_MD *p_res)
{
    UINT8 res;

    if(p_md1 == NULL)
    {
        *p_res = *p_md2;
        p_res->mode &= ~BTM_PM_MD_FORCE;

        return p_md2;
    }

    if(p_md2->mode == BTM_PM_MD_ACTIVE || p_md1->mode == BTM_PM_MD_ACTIVE)
    {
        return NULL;
    }

    /* check if force bit is involved */
    if(p_md1->mode & BTM_PM_MD_FORCE)
    {
        *p_res = *p_md1;
        p_res->mode &= ~BTM_PM_MD_FORCE;
        return p_res;
    }

    if(p_md2->mode & BTM_PM_MD_FORCE)
    {
        *p_res = *p_md2;
        p_res->mode &= ~BTM_PM_MD_FORCE;
        return p_res;
    }

    res = (p_md1->mode - 1) * BTM_PM_NUM_SET_MODES + (p_md2->mode - 1);
    res = btm_pm_md_comp_matrix[res];
    switch(res)
    {
    case BTM_PM_GET_MD1:
        *p_res = *p_md1;
        return p_md1;

    case BTM_PM_GET_MD2:
        *p_res = *p_md2;
        return p_md2;

    case BTM_PM_GET_COMP:
        p_res->mode = p_md1->mode;
        /* min of the two */
        p_res->max  = (p_md1->max < p_md2->max)? (p_md1->max) : (p_md2->max);
        /* max of the two */
        p_res->min  = (p_md1->min > p_md2->min)? (p_md1->min) : (p_md2->min);

        /* the intersection is NULL */
        if( p_res->max < p_res->min)
            return NULL;

        if(p_res->mode == BTM_PM_MD_SNIFF)
        {
            /* max of the two */
            p_res->attempt  = (p_md1->attempt > p_md2->attempt)? (p_md1->attempt) : (p_md2->attempt);
            p_res->timeout  = (p_md1->timeout > p_md2->timeout)? (p_md1->timeout) : (p_md2->timeout);
        }
        return p_res;
    }
    return NULL;
}

/*******************************************************************************
**
** Function     btm_pm_get_set_mode
** Description  get the resulting mode from the registered parties, then compare it
**              with the requested mode, if the command is from an unregistered party.
** Returns      void
**
*******************************************************************************/
static tBTM_PM_MODE btm_pm_get_set_mode(UINT8 pm_id, tBTM_PM_MCB *p_cb, tBTM_PM_PWR_MD *p_mode, tBTM_PM_PWR_MD *p_res)
{
    int   xx, loop_max;
    tBTM_PM_PWR_MD *p_md = NULL;

    if(p_mode != NULL && p_mode->mode & BTM_PM_MD_FORCE)
    {
        *p_res = *p_mode;
        p_res->mode &= ~BTM_PM_MD_FORCE;
        return p_res->mode;
    }

    if(!p_mode)
        loop_max = BTM_MAX_PM_RECORDS+1;
    else
        loop_max = BTM_MAX_PM_RECORDS;

    for( xx=0; xx<loop_max; xx++)
    {
        /* g through all the registered "set" parties */
        if(btm_cb.pm_reg_db[xx].mask & BTM_PM_REG_SET)
        {
            if(p_cb->req_mode[xx].mode == BTM_PM_MD_ACTIVE)
            {
                /* if at least one registered (SET) party says ACTIVE, stay active */
                return BTM_PM_MD_ACTIVE;
            }
            else
            {
                /* if registered parties give conflicting information, stay active */
                if( (btm_pm_compare_modes(p_md, &p_cb->req_mode[xx], p_res)) == NULL)
                    return BTM_PM_MD_ACTIVE;
                p_md = p_res;
            }
        }
    }

    /* if the resulting mode is NULL(nobody registers SET), use the requested mode */
    if(p_md == NULL)
    {
        if(p_mode)
            *p_res = *((tBTM_PM_PWR_MD *)p_mode);
        else /* p_mode is NULL when btm_pm_snd_md_req is called from btm_pm_proc_mode_change */
            return BTM_PM_MD_ACTIVE;
    }
    else
    {
        /* if the command is from unregistered party,
           compare the resulting mode from registered party*/
        if( (pm_id == BTM_PM_SET_ONLY_ID) &&
            ((btm_pm_compare_modes(p_mode, p_md, p_res)) == NULL) )
            return BTM_PM_MD_ACTIVE;
    }

    return p_res->mode;
}

/*******************************************************************************
**
** Function     btm_pm_snd_md_req
** Description  get the resulting mode and send the resuest to host controller
** Returns      tBTM_STATUS
**, BOOLEAN *p_chg_ind
*******************************************************************************/
static tBTM_STATUS btm_pm_snd_md_req(UINT8 pm_id, int link_ind, tBTM_PM_PWR_MD *p_mode)
{
    tBTM_PM_PWR_MD  md_res;
    tBTM_PM_MODE    mode;
    tBTM_PM_MCB *p_cb = &btm_cb.pm_mode_db[link_ind];
    BOOLEAN      chg_ind = FALSE;

    mode = btm_pm_get_set_mode(pm_id, p_cb, p_mode, &md_res);
    md_res.mode = mode;

#if BTM_PM_DEBUG == TRUE
    BTM_TRACE_DEBUG2( "btm_pm_snd_md_req link_ind:%d, mode: %d",
        link_ind, mode);
#endif

    if( p_cb->state == mode)
    {
        /* already in the resulting mode */
        if( (mode == BTM_PM_MD_ACTIVE) ||
            ((md_res.max >= p_cb->interval) && (md_res.min <= p_cb->interval)) )
            return BTM_CMD_STORED;
        /* Otherwise, needs to wake, then sleep */
        chg_ind = TRUE;
    }
    p_cb->chg_ind = chg_ind;

     /* cannot go directly from current mode to resulting mode. */
    if( mode != BTM_PM_MD_ACTIVE && p_cb->state != BTM_PM_MD_ACTIVE)
        p_cb->chg_ind = TRUE; /* needs to wake, then sleep */

    if(p_cb->chg_ind == TRUE) /* needs to wake first */
        md_res.mode = BTM_PM_MD_ACTIVE;
#if (BTM_SSR_INCLUDED == TRUE)
    else if(BTM_PM_MD_SNIFF == md_res.mode && p_cb->max_lat)
    {
        btsnd_hcic_sniff_sub_rate(btm_cb.acl_db[link_ind].hci_handle, p_cb->max_lat,
                                  p_cb->min_rmt_to, p_cb->min_loc_to);
        p_cb->max_lat = 0;
    }
#endif
    /* Default is failure */
    btm_cb.pm_pend_link = MAX_L2CAP_LINKS;

    /* send the appropriate HCI command */
    btm_cb.pm_pend_id   = pm_id;

#if BTM_PM_DEBUG == TRUE
    BTM_TRACE_DEBUG2("btm_pm_snd_md_req state:0x%x, link_ind: %d", p_cb->state, link_ind);
#endif
    switch(md_res.mode)
    {
    case BTM_PM_MD_ACTIVE:
        switch(p_cb->state)
        {
        case BTM_PM_MD_SNIFF:
            if (btsnd_hcic_exit_sniff_mode(btm_cb.acl_db[link_ind].hci_handle))
            {
                btm_cb.pm_pend_link = link_ind;
            }
            break;
        case BTM_PM_MD_PARK:
            if (btsnd_hcic_exit_park_mode(btm_cb.acl_db[link_ind].hci_handle))
            {
                btm_cb.pm_pend_link = link_ind;
            }
            break;
        default:
            /* Failure btm_cb.pm_pend_link = MAX_L2CAP_LINKS */
            break;
        }
        break;

    case BTM_PM_MD_HOLD:
        if (btsnd_hcic_hold_mode (btm_cb.acl_db[link_ind].hci_handle,
                                  md_res.max, md_res.min))
        {
            btm_cb.pm_pend_link = link_ind;
        }
        break;

    case BTM_PM_MD_SNIFF:
        if (btsnd_hcic_sniff_mode (btm_cb.acl_db[link_ind].hci_handle,
                                   md_res.max, md_res.min, md_res.attempt,
                                   md_res.timeout))
        {
            btm_cb.pm_pend_link = link_ind;
        }
        break;

    case BTM_PM_MD_PARK:
        if (btsnd_hcic_park_mode (btm_cb.acl_db[link_ind].hci_handle,
                                  md_res.max, md_res.min))
        {
            btm_cb.pm_pend_link = link_ind;
        }
        break;
    default:
        /* Failure btm_cb.pm_pend_link = MAX_L2CAP_LINKS */
        break;
    }

    if(btm_cb.pm_pend_link == MAX_L2CAP_LINKS)
    {
        /* the command was not sent */
#if BTM_PM_DEBUG == TRUE
        BTM_TRACE_DEBUG1( "pm_pend_link: %d",btm_cb.pm_pend_link);
#endif
        return (BTM_NO_RESOURCES);
    }

    return BTM_CMD_STARTED;
}

/*******************************************************************************
**
** Function         btm_pm_check_stored
**
** Description      This function is called when an HCI command status event occurs
**                  to check if there's any PM command issued while waiting for
**                  HCI command status.
**
** Returns          none.
**
*******************************************************************************/
static void btm_pm_check_stored(void)
{
    int     xx;
    for(xx=0; xx<MAX_L2CAP_LINKS; xx++)
    {
        if(btm_cb.pm_mode_db[xx].state & BTM_PM_STORED_MASK)
        {
            btm_cb.pm_mode_db[xx].state &= ~BTM_PM_STORED_MASK;
            BTM_TRACE_DEBUG1( "btm_pm_check_stored :%d", xx);
            btm_pm_snd_md_req(BTM_PM_SET_ONLY_ID, xx, NULL);
            break;
        }
    }
}


/*******************************************************************************
**
** Function         btm_pm_proc_cmd_status
**
** Description      This function is called when an HCI command status event occurs
**                  for power manager related commands.
**
** Input Parms      status - status of the event (HCI_SUCCESS if no errors)
**
** Returns          none.
**
*******************************************************************************/
void btm_pm_proc_cmd_status(UINT8 status)
{
    tBTM_PM_MCB     *p_cb;
    tBTM_PM_STATUS  pm_status;

    if(btm_cb.pm_pend_link >= MAX_L2CAP_LINKS)
        return;

    p_cb = &btm_cb.pm_mode_db[btm_cb.pm_pend_link];

    if(status == HCI_SUCCESS)
    {
        p_cb->state = BTM_PM_ST_PENDING;
        pm_status = BTM_PM_STS_PENDING;
#if BTM_PM_DEBUG == TRUE
        BTM_TRACE_DEBUG1( "btm_pm_proc_cmd_status new state:0x%x", p_cb->state);
#endif
    }
    else /* the command was not successfull. Stay in the same state */
    {
        pm_status = BTM_PM_STS_ERROR;
    }

    /* notify the caller is appropriate */
    if( (btm_cb.pm_pend_id != BTM_PM_SET_ONLY_ID) &&
        (btm_cb.pm_reg_db[btm_cb.pm_pend_id].mask & BTM_PM_REG_NOTIF) )
    {
        (*btm_cb.pm_reg_db[btm_cb.pm_pend_id].cback)(btm_cb.acl_db[btm_cb.pm_pend_link].remote_addr, pm_status, 0, status);
    }

    /* no pending cmd now */
#if BTM_PM_DEBUG == TRUE
    BTM_TRACE_DEBUG3( "btm_pm_proc_cmd_status state:0x%x, pm_pend_link: %d(new: %d)",
        p_cb->state, btm_cb.pm_pend_link, MAX_L2CAP_LINKS);
#endif
    btm_cb.pm_pend_link = MAX_L2CAP_LINKS;

    btm_pm_check_stored();
}

/*******************************************************************************
**
** Function         btm_pm_proc_mode_change
**
** Description      This function is called when an HCI mode change event occurs.
**
** Input Parms      hci_status - status of the event (HCI_SUCCESS if no errors)
**                  hci_handle - connection handle associated with the change
**                  mode - HCI_MODE_ACTIVE, HCI_MODE_HOLD, HCI_MODE_SNIFF, or HCI_MODE_PARK
**                  interval - number of baseband slots (meaning depends on mode)
**
** Returns          none.
**
*******************************************************************************/
void btm_pm_proc_mode_change (UINT8 hci_status, UINT16 hci_handle, UINT8 mode, UINT16 interval)
{
    tACL_CONN   *p;
    tBTM_PM_MCB *p_cb = NULL;
    int xx, yy, zz;
    tBTM_PM_STATE  old_state;
    tL2C_LCB        *p_lcb;

    /* get the index to acl_db */
    /* If Power mode change is not successful for particular ACL due to connection timeout.
     it means that LMP link for this particular ACL is no more exist. So another power mode change
     command should not be sent to controller on this particular ACL as it does not exist anymore */
    if ((xx = btm_handle_to_acl_index(hci_handle)) >= MAX_L2CAP_LINKS
       || hci_status == HCI_ERR_CONNECTION_TOUT)
    {
        BTM_TRACE_DEBUG2("btm_pm_proc_mode_change: xx: %d  hci_status: 0x%x", xx, hci_status);
        return;
    }

    p = &btm_cb.acl_db[xx];

    /*** 2035 and 2045 work around:  If mode is active and coming out of a SCO disconnect, restore packet types ***/
    if (mode == HCI_MODE_ACTIVE)
    {
        if(BTM_GetNumScoLinks() == 0)
        {
            if(p->restore_pkt_types)
    {
        BTM_TRACE_DEBUG3("btm mode change AFTER unsniffing; hci hdl 0x%x, types 0x%02x/0x%02x",
                            hci_handle, p->pkt_types_mask, p->restore_pkt_types);
        p->pkt_types_mask = p->restore_pkt_types;
        p->restore_pkt_types = 0;   /* Only exists while SCO is active */
        btsnd_hcic_change_conn_type (p->hci_handle, p->pkt_types_mask);
    }
#if (BTM_PM_SNIFF_SLOT_WORK_AROUND == TRUE)
            else
            {
                BTM_TRACE_DEBUG2("btm mode change AFTER unsniffing; hci hdl 0x%x, types 0x%02x",
                                    hci_handle, btm_cb.btm_acl_pkt_types_supported);
                btm_set_packet_types (p, btm_cb.btm_acl_pkt_types_supported);
            }
#endif
        }
#if (BTM_PM_SNIFF_SLOT_WORK_AROUND == TRUE)
        else
        {
            /* Mode changed from Sniff to Active while SCO is open. */
            /* Packet types of active mode, not sniff mode, should be used for ACL when SCO is closed. */
            p->restore_pkt_types = btm_cb.btm_acl_pkt_types_supported;

            /* Exclude packet types not supported by the peer */
            btm_acl_chk_peer_pkt_type_support (p, &p->restore_pkt_types);
        }
#endif
    }
#if (BTM_PM_SNIFF_SLOT_WORK_AROUND == TRUE)
    else if (mode == HCI_MODE_SNIFF)
    {
        BTM_TRACE_DEBUG1("btm mode change to sniff; hci hdl 0x%x use single slot",
                            hci_handle);
        btm_set_packet_types (p, (HCI_PKT_TYPES_MASK_DM1 | HCI_PKT_TYPES_MASK_DH1));
    }
#endif

    /* update control block */
    p_cb = &(btm_cb.pm_mode_db[xx]);
    old_state       = p_cb->state;
    p_cb->state     = mode;
    p_cb->interval  = interval;
#if BTM_PM_DEBUG == TRUE
    BTM_TRACE_DEBUG2( "btm_pm_proc_mode_change new state:0x%x (old:0x%x)", p_cb->state, old_state);
#endif

    if ((p_cb->state == HCI_MODE_ACTIVE) &&
        ((p_lcb = l2cu_find_lcb_by_bd_addr (p->remote_addr)) != NULL))
    {
        /* There might be any pending packets due to SNIFF or PENDING state */
        /* Trigger L2C to start transmission of the pending packets.        */
        BTM_TRACE_DEBUG0 ("btm mode change to active; check l2c_link for outgoing packets");
        l2c_link_check_send_pkts (p_lcb, NULL, NULL);

        //btu_stop_timer (&p_lcb->timer_entry);
    }

    /* notify registered parties */
    for(yy=0; yy<=BTM_MAX_PM_RECORDS; yy++)
    {
        /* set req_mode  HOLD mode->ACTIVE */
        if( (mode == BTM_PM_MD_ACTIVE) && (p_cb->req_mode[yy].mode == BTM_PM_MD_HOLD) )
            p_cb->req_mode[yy].mode = BTM_PM_MD_ACTIVE;
    }

    /* new request has been made. - post a message to BTU task */
    if(old_state & BTM_PM_STORED_MASK)
    {
#if BTM_PM_DEBUG == TRUE
        BTM_TRACE_DEBUG1( "btm_pm_proc_mode_change: Sending stored req:%d", xx);
#endif
        btm_pm_snd_md_req(BTM_PM_SET_ONLY_ID, xx, NULL);
    }
    else
    {
        for(zz=0; zz<MAX_L2CAP_LINKS; zz++)
        {
            if(btm_cb.pm_mode_db[zz].chg_ind == TRUE)
            {
#if BTM_PM_DEBUG == TRUE
                BTM_TRACE_DEBUG1( "btm_pm_proc_mode_change: Sending PM req :%d", zz);
#endif
                btm_pm_snd_md_req(BTM_PM_SET_ONLY_ID, zz, NULL);
                break;
            }
        }
    }


    /* notify registered parties */
    for(yy=0; yy<BTM_MAX_PM_RECORDS; yy++)
    {
        if(btm_cb.pm_reg_db[yy].mask & BTM_PM_REG_NOTIF)
        {
            (*btm_cb.pm_reg_db[yy].cback)( p->remote_addr, mode, interval, hci_status);
        }
    }
#if BTM_SCO_INCLUDED == TRUE
    /*check if sco disconnect  is waiting for the mode change */
    btm_sco_disc_chk_pend_for_modechange(hci_handle);
#endif

    /* If mode change was because of an active role switch or change link key */
    btm_cont_rswitch_or_chglinkkey(p, btm_find_dev(p->remote_addr), hci_status);
}

/*******************************************************************************
**
** Function         btm_pm_proc_ssr_evt
**
** Description      This function is called when an HCI sniff subrating event occurs.
**
** Returns          none.
**
*******************************************************************************/
#if (BTM_SSR_INCLUDED == TRUE)
void btm_pm_proc_ssr_evt (UINT8 *p, UINT16 evt_len)
{
    UINT8       status;
    UINT16      handle;
    UINT16      max_tx_lat, max_rx_lat;
    int         xx, yy;
    tBTM_PM_MCB *p_cb;
    tACL_CONN   *p_acl=NULL;
    UINT16      use_ssr = TRUE;

    STREAM_TO_UINT8 (status, p);

    STREAM_TO_UINT16 (handle, p);
    /* get the index to acl_db */
    if ((xx = btm_handle_to_acl_index(handle)) >= MAX_L2CAP_LINKS)
        return;

    STREAM_TO_UINT16 (max_tx_lat, p);
    STREAM_TO_UINT16 (max_rx_lat, p);
    p_cb = &(btm_cb.pm_mode_db[xx]);

    p_acl = &btm_cb.acl_db[xx];
    if(p_cb->interval == max_rx_lat)
    {
        /* using legacy sniff */
        use_ssr = FALSE;
    }

    /* notify registered parties */
    for(yy=0; yy<BTM_MAX_PM_RECORDS; yy++)
    {
        if(btm_cb.pm_reg_db[yy].mask & BTM_PM_REG_NOTIF)
        {
            if( p_acl)
            {
                (*btm_cb.pm_reg_db[yy].cback)( p_acl->remote_addr, BTM_PM_STS_SSR, use_ssr, status);
            }
        }
    }
}
#endif
#else /* BTM_PWR_MGR_INCLUDED == TRUE */

/*******************************************************************************
**
** Functions        BTM_PmRegister, BTM_SetPowerMode, and BTM_ReadPowerMode
**
** Description      Stubbed versions for BTM_PWR_MGR_INCLUDED = FALSE
**
** Returns          BTM_MODE_UNSUPPORTED.
**
*******************************************************************************/
tBTM_STATUS BTM_PmRegister (UINT8 mask, UINT8 *p_pm_id, tBTM_PM_STATUS_CBACK *p_cb)
{
    return BTM_MODE_UNSUPPORTED;
}

tBTM_STATUS BTM_SetPowerMode (UINT8 pm_id, BD_ADDR remote_bda, tBTM_PM_PWR_MD *p_mode)
{
    return BTM_MODE_UNSUPPORTED;
}

tBTM_STATUS BTM_ReadPowerMode (BD_ADDR remote_bda, tBTM_PM_MODE *p_mode)
{
    return BTM_MODE_UNSUPPORTED;
}

#endif

/*******************************************************************************
**
** Function         BTM_IsPowerManagerOn
**
** Description      This function is called to check if power manager is included.
**                  in the BTE version.
**
** Returns          BTM_PWR_MGR_INCLUDED.
**
*******************************************************************************/
BOOLEAN BTM_IsPowerManagerOn (void)
{
    return BTM_PWR_MGR_INCLUDED;
}
