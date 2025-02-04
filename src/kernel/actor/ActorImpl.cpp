/* Copyright (c) 2007-2019. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "mc/mc.h"
#include "simgrid/Exception.hpp"
#include "simgrid/s4u/Actor.hpp"
#include "simgrid/s4u/Exec.hpp"
#include "src/kernel/activity/CommImpl.hpp"
#include "src/kernel/activity/ExecImpl.hpp"
#include "src/kernel/activity/IoImpl.hpp"
#include "src/kernel/activity/SleepImpl.hpp"
#include "src/kernel/activity/SynchroRaw.hpp"
#include "src/mc/mc_replay.hpp"
#include "src/mc/remote/Client.hpp"
#include "src/simix/smx_private.hpp"
#include "src/surf/HostImpl.hpp"
#include "src/surf/cpu_interface.hpp"

#include <boost/range/algorithm.hpp>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(simix_process, simix, "Logging specific to SIMIX (process)");

/**
 * @brief Returns the current agent.
 *
 * This functions returns the currently running SIMIX process.
 *
 * @return The SIMIX process
 */
smx_actor_t SIMIX_process_self()
{
  simgrid::kernel::context::Context* self_context = simgrid::kernel::context::Context::self();

  return (self_context != nullptr) ? self_context->get_actor() : nullptr;
}

/**
 * @brief Returns whether a process has pending asynchronous communications.
 * @return true if there are asynchronous communications in this process
 * @deprecated
 */
int SIMIX_process_has_pending_comms(smx_actor_t process)
{

  return process->comms.size() > 0;
}

namespace simgrid {
namespace kernel {
namespace actor {

static unsigned long maxpid = 0;
int get_maxpid()
{
  return maxpid;
}

ActorImpl::ActorImpl(const simgrid::xbt::string& name, s4u::Host* host) : host_(host), name_(name), piface_(this)
{
  pid_           = maxpid++;
  simcall.issuer_ = this;
}

ActorImpl::~ActorImpl()
{
  if (simix_global != nullptr && this != simix_global->maestro_process) {
    if (context_.get() != nullptr) /* the actor was not start()ed yet. This happens if its host was initially off */
      context_->iwannadie = false; // don't let the simcall's yield() do a Context::stop(), to avoid infinite loops
    simgrid::kernel::actor::simcall([this] { simgrid::s4u::Actor::on_destruction(*ciface()); });
    if (context_.get() != nullptr)
      context_->iwannadie = true;
  }
}

/* Become an actor in the simulation
 *
 * Currently this can only be called by the main thread (once) and only work with some thread factories
 * (currently ThreadContextFactory).
 *
 * In the future, it might be extended in order to attach other threads created by a third party library.
 */

ActorImplPtr ActorImpl::attach(const std::string& name, void* data, s4u::Host* host,
                               const std::unordered_map<std::string, std::string>* properties)
{
  // This is mostly a copy/paste from create(), it'd be nice to share some code between those two functions.

  XBT_DEBUG("Attach process %s on host '%s'", name.c_str(), host->get_cname());

  if (not host->is_on()) {
    XBT_WARN("Cannot launch process '%s' on failed host '%s'", name.c_str(), host->get_cname());
    throw simgrid::HostFailureException(XBT_THROW_POINT, "Cannot attach actor on failed host.");
  }

  ActorImpl* actor = new ActorImpl(xbt::string(name), host);
  /* Actor data */
  actor->set_user_data(data);
  actor->code_ = nullptr;

  XBT_VERB("Create context %s", actor->get_cname());
  xbt_assert(simix_global != nullptr, "simix is not initialized, please call MSG_init first");
  actor->context_.reset(simix_global->context_factory->attach(actor));

  /* Add properties */
  if (properties != nullptr)
    actor->set_properties(*properties);

  /* Add the process to it's host process list */
  host->pimpl_->process_list_.push_back(*actor);

  /* Now insert it in the global process list and in the process to run list */
  simix_global->process_list[actor->get_pid()] = actor;
  XBT_DEBUG("Inserting [%p] %s(%s) in the to_run list", actor, actor->get_cname(), host->get_cname());
  simix_global->actors_to_run.push_back(actor);
  intrusive_ptr_add_ref(actor);

  auto* context = dynamic_cast<simgrid::kernel::context::AttachContext*>(actor->context_.get());
  xbt_assert(nullptr != context, "Not a suitable context");
  context->attach_start();

  /* The on_creation() signal must be delayed until there, where the pid and everything is set */
  simgrid::s4u::Actor::on_creation(*actor->ciface());

  return ActorImplPtr(actor);
}
/** @brief Detach an actor attached with `attach()`
 *
 *  This is called when the current actor has finished its job.
 *  Used in the main thread, it waits for the simulation to finish before returning. When it returns, the other
 *  simulated actors and the maestro are destroyed.
 */
void ActorImpl::detach()
{
  auto* context = dynamic_cast<context::AttachContext*>(context::Context::self());
  if (context == nullptr)
    xbt_die("Not a suitable context");

  context->get_actor()->cleanup();
  context->attach_stop();
}

void ActorImpl::cleanup()
{
  finished_ = true;

  if (has_to_auto_restart() && not get_host()->is_on()) {
    XBT_DEBUG("Insert host %s to watched_hosts because it's off and %s needs to restart", get_host()->get_cname(),
              get_cname());
    watched_hosts.insert(get_host()->get_name());
  }

  if (on_exit) {
    // Execute the termination callbacks
    bool failed = context_->iwannadie;
    for (auto exit_fun = on_exit->crbegin(); exit_fun != on_exit->crend(); ++exit_fun)
      (*exit_fun)(failed);
    on_exit.reset();
  }
  undaemonize();

  /* cancel non-blocking activities */
  for (auto activity : comms)
    boost::static_pointer_cast<activity::CommImpl>(activity)->cancel();
  comms.clear();

  XBT_DEBUG("%s@%s(%ld) should not run anymore", get_cname(), get_host()->get_cname(), get_pid());

  if (this == simix_global->maestro_process) /* Do not cleanup maestro */
    return;

  XBT_DEBUG("Cleanup actor %s (%p), waiting synchro %p", get_cname(), this, waiting_synchro.get());

  /* Unregister from the kill timer if any */
  if (kill_timer != nullptr) {
    kill_timer->remove();
    kill_timer = nullptr;
  }

  simix_global->mutex.lock();

  simix_global->process_list.erase(pid_);
  if (host_ && host_process_list_hook.is_linked())
    simgrid::xbt::intrusive_erase(host_->pimpl_->process_list_, *this);
  if (not smx_destroy_list_hook.is_linked()) {
#if SIMGRID_HAVE_MC
    xbt_dynar_push_as(simix_global->dead_actors_vector, ActorImpl*, this);
#endif
    simix_global->actors_to_destroy.push_back(*this);
  }

  simix_global->mutex.unlock();

  context_->iwannadie = false; // don't let the simcall's yield() do a Context::stop(), to avoid infinite loops
  simgrid::kernel::actor::simcall([this] { simgrid::s4u::Actor::on_termination(*ciface()); });
  context_->iwannadie = true;
}

void ActorImpl::exit()
{
  context_->iwannadie = true;
  suspended_          = false;
  exception_          = nullptr;

  /* destroy the blocking synchro if any */
  if (waiting_synchro != nullptr) {
    waiting_synchro->cancel();
    waiting_synchro->state_ = SIMIX_FAILED;

    activity::ExecImplPtr exec   = boost::dynamic_pointer_cast<activity::ExecImpl>(waiting_synchro);
    activity::CommImplPtr comm   = boost::dynamic_pointer_cast<activity::CommImpl>(waiting_synchro);

    if (exec != nullptr) {
      exec->clean_action();
    } else if (comm != nullptr) {
      comms.remove(waiting_synchro);
      // Remove first occurrence of &actor->simcall:
      auto i = boost::range::find(waiting_synchro->simcalls_, &simcall);
      if (i != waiting_synchro->simcalls_.end())
        waiting_synchro->simcalls_.remove(&simcall);
    } else {
      activity::ActivityImplPtr(waiting_synchro)->finish();
    }

    waiting_synchro = nullptr;
  }

  // Forcefully kill the actor if its host is turned off. Not a HostFailureException because you should not survive that
  this->throw_exception(std::make_exception_ptr(ForcefulKillException(host_->is_on() ? "exited" : "host failed")));
}

void ActorImpl::kill(ActorImpl* actor)
{
  if (actor->finished_) {
    XBT_DEBUG("Ignoring request to kill actor %s@%s that is already dead", actor->get_cname(),
              actor->host_->get_cname());
    return;
  }

  XBT_DEBUG("Actor '%s'@%s is killing actor '%s'@%s", get_cname(), host_ ? host_->get_cname() : "", actor->get_cname(),
            actor->host_ ? actor->host_->get_cname() : "");

  actor->exit();

  if (std::find(begin(simix_global->actors_to_run), end(simix_global->actors_to_run), actor) ==
          end(simix_global->actors_to_run) &&
      actor != this) {
    XBT_DEBUG("Inserting %s in the to_run list", actor->get_cname());
    simix_global->actors_to_run.push_back(actor);
  }
}

void ActorImpl::kill_all()
{
  for (auto const& kv : simix_global->process_list)
    if (kv.second != this)
      this->kill(kv.second);
}

void ActorImpl::set_kill_time(double kill_time)
{
  if (kill_time <= SIMIX_get_clock())
    return;
  XBT_DEBUG("Set kill time %f for actor %s@%s", kill_time, get_cname(), host_->get_cname());
  kill_timer = simix::Timer::set(kill_time, [this] {
    this->exit();
    kill_timer = nullptr;
  });
}

double ActorImpl::get_kill_time()
{
  return kill_timer ? kill_timer->get_date() : 0;
}

void ActorImpl::yield()
{
  XBT_DEBUG("Yield actor '%s'", get_cname());

  /* Go into sleep and return control to maestro */
  context_->suspend();

  /* Ok, maestro returned control to us */
  XBT_DEBUG("Control returned to me: '%s'", get_cname());

  if (context_->iwannadie) {
    XBT_DEBUG("Actor %s@%s is dead", get_cname(), host_->get_cname());
    // throw simgrid::kernel::context::ForcefulKillException(); Does not seem to properly kill the actor
    context_->stop();
    THROW_IMPOSSIBLE;
  }

  if (suspended_) {
    XBT_DEBUG("Hey! I'm suspended.");

    xbt_assert(exception_ == nullptr, "Gasp! This exception may be lost by subsequent calls.");
    suspended_ = false;
    suspend(this);
  }

  if (exception_ != nullptr) {
    XBT_DEBUG("Wait, maestro left me an exception");
    std::exception_ptr exception = std::move(exception_);
    exception_                   = nullptr;
    std::rethrow_exception(std::move(exception));
  }

  if (SMPI_switch_data_segment && not finished_) {
    SMPI_switch_data_segment(iface());
  }
}

/** This actor will be terminated automatically when the last non-daemon actor finishes */
void ActorImpl::daemonize()
{
  if (not daemon_) {
    daemon_ = true;
    simix_global->daemons.push_back(this);
  }
}

void ActorImpl::undaemonize()
{
  if (daemon_) {
    auto& vect = simix_global->daemons;
    auto it    = std::find(vect.begin(), vect.end(), this);
    xbt_assert(it != vect.end(), "The dying daemon is not a daemon after all. Please report that bug.");
    /* Don't move the whole content since we don't really care about the order */

    std::swap(*it, vect.back());
    vect.pop_back();
    daemon_ = false;
  }
}

s4u::Actor* ActorImpl::restart()
{
  xbt_assert(this != simix_global->maestro_process, "Restarting maestro is not supported");

  XBT_DEBUG("Restarting actor %s on %s", get_cname(), host_->get_cname());

  // retrieve the arguments of the old actor
  ProcessArg arg = ProcessArg(host_, this);

  // kill the old actor
  context::Context::self()->get_actor()->kill(this);

  // start the new actor
  ActorImplPtr actor =
      ActorImpl::create(arg.name, std::move(arg.code), arg.data, arg.host, arg.properties.get(), nullptr);
  *actor->on_exit = std::move(*arg.on_exit);
  actor->set_kill_time(arg.kill_time);
  actor->set_auto_restart(arg.auto_restart);

  return actor->ciface();
}

void ActorImpl::suspend(ActorImpl* issuer)
{
  if (suspended_) {
    XBT_DEBUG("Actor '%s' is already suspended", get_cname());
    return;
  }

  suspended_ = true;

  /* If the suspended actor is waiting on a sync, suspend its synchronization. */
  if (waiting_synchro == nullptr) {
    activity::ExecImpl* exec = new activity::ExecImpl();
    exec->set_name("suspend").set_host(host_).set_flops_amount(0.0).start();
    waiting_synchro = activity::ExecImplPtr(exec);

    waiting_synchro->simcalls_.push_back(&simcall);
  }
  waiting_synchro->suspend();
}

void ActorImpl::resume()
{
  XBT_IN("actor = %p", this);

  if (context_->iwannadie) {
    XBT_VERB("Ignoring request to suspend an actor that is currently dying.");
    return;
  }

  if (not suspended_)
    return;
  suspended_ = false;

  /* resume the synchronization that was blocking the resumed actor. */
  if (waiting_synchro)
    waiting_synchro->resume();

  XBT_OUT();
}

activity::ActivityImplPtr ActorImpl::join(ActorImpl* actor, double timeout)
{
  activity::ActivityImplPtr sleep = this->sleep(timeout);
  SIMIX_process_on_exit(actor, [sleep](bool) {
    if (sleep->surf_action_)
      sleep->surf_action_->finish(resource::Action::State::FINISHED);
  });
  return sleep;
}

activity::ActivityImplPtr ActorImpl::sleep(double duration)
{
  if (not host_->is_on())
    throw_exception(std::make_exception_ptr(simgrid::HostFailureException(
        XBT_THROW_POINT, std::string("Host ") + host_->get_cname() + " failed, you cannot sleep there.")));

  activity::SleepImpl* sleep = new activity::SleepImpl();
  (*sleep).set_name("sleep").set_host(host_).set_duration(duration).start();
  return activity::SleepImplPtr(sleep);
}

void ActorImpl::throw_exception(std::exception_ptr e)
{
  exception_ = e;

  if (suspended_)
    resume();

  /* cancel the blocking synchro if any */
  if (waiting_synchro) {
    waiting_synchro->cancel();

    activity::CommImplPtr comm = boost::dynamic_pointer_cast<activity::CommImpl>(waiting_synchro);

    if (comm != nullptr)
      comms.remove(comm);

    waiting_synchro = nullptr;
  }
}

void ActorImpl::simcall_answer()
{
  if (this != simix_global->maestro_process){
    XBT_DEBUG("Answer simcall %s (%d) issued by %s (%p)", SIMIX_simcall_name(simcall.call_), (int)simcall.call_,
              get_cname(), this);
    simcall.call_ = SIMCALL_NONE;
    xbt_assert(not XBT_LOG_ISENABLED(simix_process, xbt_log_priority_debug) ||
                   std::find(begin(simix_global->actors_to_run), end(simix_global->actors_to_run), this) ==
                       end(simix_global->actors_to_run),
               "Actor %p should not exist in actors_to_run!", this);
    simix_global->actors_to_run.push_back(this);
  }
}

void ActorImpl::set_host(s4u::Host* dest)
{
  xbt::intrusive_erase(host_->pimpl_->process_list_, *this);
  host_ = dest;
  dest->pimpl_->process_list_.push_back(*this);
}

ActorImplPtr ActorImpl::init(const std::string& name, s4u::Host* host)
{
  ActorImpl* actor = new ActorImpl(xbt::string(name), host);
  actor->set_ppid(this->pid_);

  intrusive_ptr_add_ref(actor);
  /* The on_creation() signal must be delayed until there, where the pid and everything is set */
  s4u::Actor::on_creation(*actor->ciface());

  return ActorImplPtr(actor);
}

ActorImpl* ActorImpl::start(const simix::ActorCode& code)
{
  xbt_assert(code && host_ != nullptr, "Invalid parameters");

  if (not host_->is_on()) {
    XBT_WARN("Cannot launch actor '%s' on failed host '%s'", name_.c_str(), host_->get_cname());
    intrusive_ptr_release(this);
    throw simgrid::HostFailureException(XBT_THROW_POINT, "Cannot start actor on failed host.");
  }

  this->code_ = code;
  XBT_VERB("Create context %s", get_cname());
  context_.reset(simix_global->context_factory->create_context(simix::ActorCode(code), this));

  XBT_DEBUG("Start context '%s'", get_cname());

  /* Add the actor to its host's actor list */
  host_->pimpl_->process_list_.push_back(*this);
  simix_global->process_list[pid_] = this;

  /* Now insert it in the global actor list and in the actor to run list */
  XBT_DEBUG("Inserting [%p] %s(%s) in the to_run list", this, get_cname(), host_->get_cname());
  simix_global->actors_to_run.push_back(this);

  return this;
}

ActorImplPtr ActorImpl::create(const std::string& name, const simix::ActorCode& code, void* data, s4u::Host* host,
                               const std::unordered_map<std::string, std::string>* properties, ActorImpl* parent_actor)
{
  XBT_DEBUG("Start actor %s@'%s'", name.c_str(), host->get_cname());

  ActorImplPtr actor;
  if (parent_actor != nullptr)
    actor = parent_actor->init(xbt::string(name), host);
  else
    actor = SIMIX_process_self()->init(xbt::string(name), host);

  /* actor data */
  actor->set_user_data(data);

  /* Add properties */
  if (properties != nullptr)
    actor->set_properties(*properties);

  actor->start(code);

  return actor;
}

void create_maestro(const std::function<void()>& code)
{
  /* Create maestro actor and initialize it */
  ActorImpl* maestro = new ActorImpl(xbt::string(""), /*host*/ nullptr);

  if (not code) {
    maestro->context_.reset(simix_global->context_factory->create_context(simix::ActorCode(), maestro));
  } else {
    maestro->context_.reset(simix_global->context_factory->create_maestro(simix::ActorCode(code), maestro));
  }

  maestro->simcall.issuer_      = maestro;
  simix_global->maestro_process = maestro;
}

} // namespace actor
} // namespace kernel
} // namespace simgrid

void SIMIX_process_detach() // deprecated v3.25
{
  simgrid::kernel::actor::ActorImpl::detach();
}

smx_actor_t SIMIX_process_attach(const char* name, void* data, const char* hostname,
                                 std::unordered_map<std::string, std::string>* properties,
                                 smx_actor_t /*parent_process*/) // deprecated 3.25
{
  return simgrid::kernel::actor::ActorImpl::attach(name, data, sg_host_by_name(hostname), properties).get();
}

int SIMIX_process_count()
{
  return simix_global->process_list.size();
}

void* SIMIX_process_self_get_data() // deprecated
{
  smx_actor_t self = SIMIX_process_self();

  if (self == nullptr) {
    return nullptr;
  }
  return self->get_user_data();
}

void SIMIX_process_self_set_data(void* data) // deprecated
{
  SIMIX_process_self()->set_user_data(data);
}

/* needs to be public and without simcall because it is called
   by exceptions and logging events */
const char* SIMIX_process_self_get_name()
{

  smx_actor_t process = SIMIX_process_self();
  if (process == nullptr || process == simix_global->maestro_process)
    return "maestro";

  return process->get_cname();
}

/**
 * @brief Calling this function makes the process to yield.
 *
 * Only the current process can call this function, giving back the control to maestro.
 *
 * @param self the current process
 */

/** @brief Returns the list of processes to run.
 * @deprecated
 */
const std::vector<smx_actor_t>& simgrid::simix::process_get_runnable()
{
  return simix_global->actors_to_run;
}

/** @brief Returns the process from PID. */
smx_actor_t SIMIX_process_from_PID(aid_t PID)
{
  auto item = simix_global->process_list.find(PID);
  if (item == simix_global->process_list.end()) {
    for (auto& a : simix_global->actors_to_destroy)
      if (a.get_pid() == PID)
        return &a;
    return nullptr; // Not found, even in the trash
  }
  return item->second;
}

void SIMIX_process_on_exit(smx_actor_t actor, int_f_pvoid_pvoid_t fun, void* data)
{
  SIMIX_process_on_exit(actor, [fun, data](bool failed) {
    intptr_t status = failed ? SMX_EXIT_FAILURE : SMX_EXIT_SUCCESS;
    fun(reinterpret_cast<void*>(status), data);
  });
}

void SIMIX_process_on_exit(smx_actor_t actor, const std::function<void(int, void*)>& fun, void* data)
{
  SIMIX_process_on_exit(actor, [fun, data](bool failed) { fun(failed ? SMX_EXIT_FAILURE : SMX_EXIT_SUCCESS, data); });
}

void SIMIX_process_on_exit(smx_actor_t actor, const std::function<void(bool /*failed*/)>& fun)
{
  xbt_assert(actor, "current process not found: are you in maestro context ?");
  actor->on_exit->emplace_back(fun);
}

/** @brief Restart a process, starting it again from the beginning. */
/**
 * @ingroup simix_process_management
 * @brief Creates and runs a new SIMIX process.
 *
 * The structure and the corresponding thread are created and put in the list of ready processes.
 *
 * @param name a name for the process. It is for user-level information and can be nullptr.
 * @param code the main function of the process
 * @param data a pointer to any data one may want to attach to the new object. It is for user-level information and can
 * be nullptr.
 * It can be retrieved with the method ActorImpl::getUserData().
 * @param host where the new agent is executed.
 * @param properties the properties of the process
 */
smx_actor_t simcall_process_create(const std::string& name, const simgrid::simix::ActorCode& code, void* data,
                                   sg_host_t host, std::unordered_map<std::string, std::string>* properties)
{
  smx_actor_t self = SIMIX_process_self();
  return simgrid::kernel::actor::simcall([&name, &code, data, host, properties, self] {
    return simgrid::kernel::actor::ActorImpl::create(name, code, data, host, properties, self).get();
  });
}

void simcall_process_set_data(smx_actor_t process, void* data)
{
  simgrid::kernel::actor::simcall([process, data] { process->set_user_data(data); });
}
