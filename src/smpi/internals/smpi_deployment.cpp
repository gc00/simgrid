/* Copyright (c) 2004-2019. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "smpi_host.hpp"
#include "private.hpp"
#include "simgrid/s4u/Engine.hpp"
#include "smpi_comm.hpp"
#include <map>

XBT_LOG_EXTERNAL_CATEGORY(smpi);

namespace simgrid {
namespace smpi {
namespace app {

static int universe_size = 0;

class Instance {
public:
  Instance(const std::string& name, int max_no_processes, MPI_Comm comm)
      : name_(name), size_(max_no_processes), comm_world_(comm)
  {
    MPI_Group group = new simgrid::smpi::Group(size_);
    comm_world_     = new simgrid::smpi::Comm(group, nullptr, 0, -1);
    //  FIXME : using MPI_Attr_put with MPI_UNIVERSE_SIZE is forbidden and we make it a no-op (which triggers a warning
    //  as MPI_ERR_ARG is returned). Directly calling Comm::attr_put breaks for now, as MPI_UNIVERSE_SIZE,is <0
    //  instance.comm_world->attr_put<simgrid::smpi::Comm>(MPI_UNIVERSE_SIZE, reinterpret_cast<void*>(instance.size));

    universe_size += max_no_processes;
  }

  const std::string name_;
  unsigned int size_;
  std::vector<simgrid::s4u::ActorPtr> present_processes_;
  unsigned int finalized_ranks_ = 0;
  MPI_Comm comm_world_;
};
}
}
}

using simgrid::smpi::app::Instance;

static std::map<std::string, Instance> smpi_instances;

/** @ingroup smpi_simulation
 * @brief Registers a running instance of a MPI program.
 *
 * @param name the reference name of the function.
 * @param code either the main mpi function
 *             (must have a int ..(int argc, char *argv[]) prototype) or nullptr
 *             (if the function deployment is managed somewhere else —
 *              e.g., when deploying manually or using smpirun)
 * @param num_processes the size of the instance we want to deploy
 */
void SMPI_app_instance_register(const char *name, xbt_main_func_t code, int num_processes)
{
  if (code != nullptr) // When started with smpirun, we will not execute a function
    simgrid::s4u::Engine::get_instance()->register_function(name, code);

  static bool already_called = false;
  if (not already_called) {
    already_called = true;
    for (auto const& host : simgrid::s4u::Engine::get_instance()->get_all_hosts())
      host->extension_set(new simgrid::smpi::Host(host));
  }

  Instance instance(std::string(name), num_processes, MPI_COMM_NULL);

  smpi_instances.insert(std::pair<std::string, Instance>(name, instance));
}

void smpi_deployment_register_process(const std::string& instance_id, int rank, simgrid::s4u::ActorPtr actor)
{
  Instance& instance = smpi_instances.at(instance_id);
  instance.present_processes_.push_back(actor);
  instance.comm_world_->group()->set_mapping(actor, rank);
}

void smpi_deployment_unregister_process(const std::string& instance_id)
{
  Instance& instance = smpi_instances.at(instance_id);
  instance.finalized_ranks_++;

  if (instance.finalized_ranks_ == instance.size_) {
    instance.present_processes_.clear();
    simgrid::smpi::Comm::destroy(instance.comm_world_);
    smpi_instances.erase(instance_id);
  }
}

MPI_Comm* smpi_deployment_comm_world(const std::string& instance_id)
{
  if (smpi_instances
          .empty()) { // no instance registered, we probably used smpirun. (FIXME: I guess this never happens for real)
    return nullptr;
  }
  Instance& instance = smpi_instances.at(instance_id);
  return &instance.comm_world_;
}

void smpi_deployment_cleanup_instances(){
  for (auto const& item : smpi_instances) {
    XBT_CINFO(smpi, "Stalling SMPI instance: %s. Do all your MPI ranks call MPI_Finalize()?", item.first.c_str());
    Instance instance = item.second;
    instance.present_processes_.clear();
    simgrid::smpi::Comm::destroy(instance.comm_world_);
  }
  smpi_instances.clear();
}

int smpi_get_universe_size()
{
  return simgrid::smpi::app::universe_size;
}
