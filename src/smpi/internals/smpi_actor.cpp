/* Copyright (c) 2009-2019. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/smpi/include/smpi_actor.hpp"
#include "mc/mc.h"
#include "smpi_comm.hpp"
#include "smpi_info.hpp"
#include "src/mc/mc_replay.hpp"
#include "src/simix/smx_private.hpp"

#if HAVE_PAPI
#include "papi.h"
extern std::string papi_default_config_name;
#endif

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(smpi_process, smpi, "Logging specific to SMPI (kernel)");

namespace simgrid {
namespace smpi {

ActorExt::ActorExt(s4u::ActorPtr actor) : actor_(actor)
{
  mailbox_         = s4u::Mailbox::by_name("SMPI-" + std::to_string(actor_->get_pid()));
  mailbox_small_   = s4u::Mailbox::by_name("small-" + std::to_string(actor_->get_pid()));
  mailboxes_mutex_ = s4u::Mutex::create();
  timer_           = xbt_os_timer_new();
  state_           = SmpiProcessState::UNINITIALIZED;
  info_env_        = MPI_INFO_NULL;
  if (MC_is_active())
    MC_ignore_heap(timer_, xbt_os_timer_size());

#if HAVE_PAPI
  if (not simgrid::config::get_value<std::string>("smpi/papi-events").empty()) {
    // TODO: Implement host/process/thread based counters. This implementation
    // just always takes the values passed via "default", like this:
    // "default:COUNTER1:COUNTER2:COUNTER3;".
    auto it = units2papi_setup.find(papi_default_config_name);
    if (it != units2papi_setup.end()) {
      papi_event_set_    = it->second.event_set;
      papi_counter_data_ = it->second.counter_data;
      XBT_DEBUG("Setting PAPI set for process %li", actor->get_pid());
    } else {
      papi_event_set_ = PAPI_NULL;
      XBT_DEBUG("No PAPI set for process %li", actor->get_pid());
    }
  }
#endif
}

ActorExt::~ActorExt()
{
  TRACE_smpi_finalize(actor_->get_pid());

  if (comm_self_ != MPI_COMM_NULL)
    simgrid::smpi::Comm::destroy(comm_self_);
  if (comm_intra_ != MPI_COMM_NULL)
    simgrid::smpi::Comm::destroy(comm_intra_);
  xbt_os_timer_free(timer_);
}

/** @brief Prepares the current process for termination. */
void ActorExt::finalize()
{
  state_ = SmpiProcessState::FINALIZED;
  XBT_DEBUG("<%ld> Process left the game", actor_->get_pid());

  smpi_deployment_unregister_process(instance_id_);
}

/** @brief Check if a process is finalized */
int ActorExt::finalized()
{
  return (state_ == SmpiProcessState::FINALIZED);
}

/** @brief Check if a process is partially initialized already */
int ActorExt::initializing()
{
  return (state_ == SmpiProcessState::INITIALIZING);
}

/** @brief Check if a process is initialized */
int ActorExt::initialized()
{
  // TODO cheinrich: Check if we still need this. This should be a global condition, not for a
  // single process ... ?
  return (state_ == SmpiProcessState::INITIALIZED);
}

/** @brief Mark a process as initialized (=MPI_Init called) */
void ActorExt::mark_as_initialized()
{
  if (state_ != SmpiProcessState::FINALIZED)
    state_ = SmpiProcessState::INITIALIZED;
}

void ActorExt::set_replaying(bool value)
{
  if (state_ != SmpiProcessState::FINALIZED)
    replaying_ = value;
}

bool ActorExt::replaying()
{
  return replaying_;
}

s4u::ActorPtr ActorExt::get_actor()
{
  return actor_;
}

/**
 * @brief Returns a structure that stores the location (filename + linenumber) of the last calls to MPI_* functions.
 *
 * @see smpi_trace_set_call_location
 */
smpi_trace_call_location_t* ActorExt::call_location()
{
  return &trace_call_loc_;
}

void ActorExt::set_privatized_region(smpi_privatization_region_t region)
{
  privatized_region_ = region;
}

smpi_privatization_region_t ActorExt::privatized_region()
{
  return privatized_region_;
}

MPI_Comm ActorExt::comm_world()
{
  return comm_world_ == nullptr ? MPI_COMM_NULL : *comm_world_;
}

s4u::MutexPtr ActorExt::mailboxes_mutex()
{
  return mailboxes_mutex_;
}

#if HAVE_PAPI
int ActorExt::papi_event_set()
{
  return papi_event_set_;
}

papi_counter_t& ActorExt::papi_counters()
{
  return papi_counter_data_;
}
#endif

xbt_os_timer_t ActorExt::timer()
{
  return timer_;
}

void ActorExt::simulated_start()
{
  simulated_ = SIMIX_get_clock();
}

double ActorExt::simulated_elapsed()
{
  return SIMIX_get_clock() - simulated_;
}

MPI_Comm ActorExt::comm_self()
{
  if (comm_self_ == MPI_COMM_NULL) {
    MPI_Group group = new Group(1);
    comm_self_      = new Comm(group, nullptr);
    group->set_mapping(actor_, 0);
  }
  return comm_self_;
}

MPI_Info ActorExt::info_env()
{
  if (info_env_==MPI_INFO_NULL)
    info_env_=new Info();
  return info_env_;
}

MPI_Comm ActorExt::comm_intra()
{
  return comm_intra_;
}

void ActorExt::set_comm_intra(MPI_Comm comm)
{
  comm_intra_ = comm;
}

void ActorExt::set_sampling(int s)
{
  sampling_ = s;
}

int ActorExt::sampling()
{
  return sampling_;
}

void ActorExt::init()
{
  xbt_assert(smpi_get_universe_size() != 0, "SimGrid was not initialized properly before entering MPI_Init. "
                                            "Aborting, please check compilation process and use smpirun.");

  simgrid::s4u::ActorPtr self = simgrid::s4u::Actor::self();
  // cheinrich: I'm not sure what the impact of the SMPI_switch_data_segment on this call is. I moved
  // this up here so that I can set the privatized region before the switch.
  ActorExt* ext = smpi_process();
  // if we are in MPI_Init and argc handling has already been done.
  if (ext->initialized())
    return;

  if (smpi_privatize_global_variables == SmpiPrivStrategies::MMAP) {
    /* Now using the segment index of this process  */
    ext->set_privatized_region(smpi_init_global_memory_segment_process());
    /* Done at the process's creation */
    SMPI_switch_data_segment(self);
  }

  ext->instance_id_ = self->get_property("instance_id");
  const int rank    = xbt_str_parse_int(self->get_property("rank"), "Cannot parse rank");

  ext->state_ = SmpiProcessState::INITIALIZING;
  smpi_deployment_register_process(ext->instance_id_, rank, self);

  ext->comm_world_ = smpi_deployment_comm_world(ext->instance_id_);

  // set the process attached to the mailbox
  ext->mailbox_small_->set_receiver(ext->actor_);
  XBT_DEBUG("<%ld> SMPI process has been initialized: %p", ext->actor_->get_pid(), ext->actor_.get());
}

int ActorExt::get_optind()
{
  return optind_;
}

void ActorExt::set_optind(int new_optind)
{
  optind_ = new_optind;
}

} // namespace smpi
} // namespace simgrid
