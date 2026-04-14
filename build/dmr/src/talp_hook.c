/**
 * @file talp_hook.c
 * @brief Helper code for integration with DLB's TALP 
 *
 * Contains logic used for integrating with DLB's TALP,
 * if it is included in compilation.
 */

#include "dmr.h"
#include "dmr_internal.h"

#if defined(COMPILED_WITH_TALP)

static bool talp_failure = false;

static dlb_monitor_t *monitor_ins = NULL;
static dlb_monitor_t *monitor_acc = NULL;

/**
 * @brief Collect TALP data from a given dlb_monitor_t and return it
 */
static TALPInfo get_CE_info(dlb_monitor_t *the_monitor, bool reset_region)
{
    TALPInfo talp_info;
    talp_info.communication_efficiency = -1.0;
    talp_info.talp_etime = -1.0;

    if(talp_failure || !the_monitor)
    {
        return talp_info;
    }

    dlb_pop_metrics_t pop_metrics;
    DLB_MonitoringRegionStop(the_monitor);

    // Collective operation
    int err = DLB_TALP_CollectPOPMetrics(the_monitor, &pop_metrics);

    if(reset_region)
    {
        /*
        * WARNING: The monitoring region must be stopped when resetting.
        * Otherwise will get very spammy warning messages
        **/
        DLB_MonitoringRegionReset(the_monitor);
    }

    DLB_MonitoringRegionStart(the_monitor);

    if(err == DLB_SUCCESS)
    {
        talp_info.communication_efficiency = pop_metrics.mpi_communication_efficiency;
        talp_info.talp_etime = pop_metrics.elapsed_time;
    }
    else
    {
        dmr_error("Some issue occurred collecting TALP metrics.\n");
    }

    return talp_info;
}

#endif

/**
 * @brief Print instantaneous TALP info for analytics, or empty function if compiled without TALP
 * 
 * The 'instantaneous' TALP monitoring region takes measurements from one dmr_check to the next, resetting
 * each time information is printed.
 */
void print_talp_CE_ins(DMRState *dmr_state, DMRControllerState *controller_state)
{
#if defined(COMPILED_WITH_TALP)
    TALPInfo talp_info = get_CE_info(monitor_ins, true);
    double communication_efficiency = talp_info.communication_efficiency;

    // Error case
    if(communication_efficiency < 0)
    {
        return;
    }
    
    if(dmr_state->is_root_process)
    {
        dmr_analytics(dmr_state, controller_state, get_global_time(), communication_efficiency, __func__, "TALP_CHECK_CE_INS", false);
    }
#endif
}

/**
 * @brief Print accumulated TALP info for analytics, or empty function if compiled without TALP
 * 
 * Does NOT reset the accumulated TALP region.
 * See @ref get_talp_info_acc for description of the accumulated TALP region
 */
void print_talp_CE_acc(DMRState *dmr_state, DMRControllerState *controller_state)
{
#if defined(COMPILED_WITH_TALP)

    TALPInfo talp_info = get_CE_info(monitor_acc, false);
    double communication_efficiency = talp_info.communication_efficiency;

    // Error case
    if(communication_efficiency < 0)
    {
        return;
    }

    if(dmr_state->is_root_process)
    {
        dmr_analytics(dmr_state, controller_state, get_global_time(), communication_efficiency, __func__, "TALP_CHECK_CE_ACC", false);
    }
#endif
}

/**
 * @brief Get accumulated TALP CE, or just return TALPInfo with -1.0 values if compiled without TALP
 * 
 * The 'accumulated' TALP monitoring region starts measuring at the first dmr_check and returns
 * metrics whenever needed by policies (slowed by inhibitor); the region resets when a policy 
 * requests the data (i.e. this function is called)
 */
TALPInfo get_talp_info_acc(DMRState *dmr_state, DMRControllerState *controller_state)
{
    TALPInfo talp_info;
    talp_info.communication_efficiency = -1.0;
    talp_info.talp_etime = -1.0;

#if defined(COMPILED_WITH_TALP)

    talp_info = get_CE_info(monitor_acc, true);
    double communication_efficiency = talp_info.communication_efficiency;

    // Error case
    if(communication_efficiency < 0)
    {
        // Print error at the level of the policy using this function
        return talp_info;
    }

    if(dmr_state->is_root_process)
    {
        dmr_analytics(dmr_state, controller_state, get_global_time(), communication_efficiency, __func__, "TALP_CHECK_CE_ACC", false);
    }

#endif

    return talp_info;
}

/**
 * @brief Return whether or not we have compiled with TALP
 */
bool talp_enabled(void)
{
#if defined(COMPILED_WITH_TALP)
    return true;
#else
    return false;
#endif
}

/**
 * @brief Start the TALP monitoring region(s) if not already started, or empty function if not compiled with TALP
 */
void start_monitoring_regions(DMRState *dmr_state)
{
#if defined(COMPILED_WITH_TALP)

    if(talp_failure)
    {
        return;
    }

    // The instantaneous monitoring region is only needed for analytics purposes
    if(should_print_analytics() && !monitor_ins)
    {
        monitor_ins = DLB_MonitoringRegionRegister(DLB_MONITOR_NAME_INS);
        int err = DLB_MonitoringRegionStart(monitor_ins);

        if(err != DLB_SUCCESS)
        {
            if(dmr_state->is_root_process)
            {
                dmr_error("TALP error when starting monitoring region (INS). Make sure to LD_PRELOAD libdlb_mpi.so\n");
            }

            talp_failure = true;
        }
    }

    if(!monitor_acc)
    {
        monitor_acc = DLB_MonitoringRegionRegister(DLB_MONITOR_NAME_ACC);
        int err = DLB_MonitoringRegionStart(monitor_acc);
        
        if(err != DLB_SUCCESS)
        {
            if(dmr_state->is_root_process)
            {
                dmr_error("TALP error when starting monitoring region (ACC). Make sure to LD_PRELOAD libdlb_mpi.so\n");
            }
            
            talp_failure = true;
        }
    }

#endif
}

/**
 * @brief Stop DMR monitoring regions, or empty function if not compiled with TALP
 */
void stop_monitoring_regions(void)
{
#if defined(COMPILED_WITH_TALP)
    if(monitor_ins)
    {
        DLB_MonitoringRegionStop(monitor_ins);
    }

    if(monitor_acc)
    {
        DLB_MonitoringRegionStop(monitor_acc);
    }
#endif
}

/**
 * @brief Finalize DLB and stop monitoring regions, or empty function if not compiled with TALP
 */
void stop_and_finalize_dlb(void)
{
#if defined(COMPILED_WITH_TALP)
    debug_output("About to stop monitoring regions and finalize DLB\n");

    stop_monitoring_regions();
    DLB_Finalize();
    
    debug_output("Stopped monitoring regions and finalized DLB\n");
#endif
}