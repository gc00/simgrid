/* Copyright (c) 2011-2019. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/mc/checker/LivenessChecker.hpp"
#include "src/mc/Session.hpp"
#include "src/mc/mc_config.hpp"
#include "src/mc/mc_exit.hpp"
#include "src/mc/mc_private.hpp"
#include "src/mc/mc_request.hpp"
#include "src/mc/mc_smx.hpp"

#include <boost/range/algorithm.hpp>
#include <cstring>

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(mc_liveness, mc, "Logging specific to algorithms for liveness properties verification");

/********* Static functions *********/

namespace simgrid {
namespace mc {

VisitedPair::VisitedPair(int pair_num, xbt_automaton_state_t automaton_state,
                         std::shared_ptr<const std::vector<int>> atomic_propositions,
                         std::shared_ptr<simgrid::mc::State> graph_state)
    : num(pair_num), automaton_state(automaton_state)
{
  simgrid::mc::RemoteClient* process = &(mc_model_checker->process());

  this->graph_state = std::move(graph_state);
  if(this->graph_state->system_state == nullptr)
    this->graph_state->system_state = std::make_shared<simgrid::mc::Snapshot>(pair_num);
  this->heap_bytes_used = mmalloc_get_bytes_used_remote(process->get_heap()->heaplimit, process->get_malloc_info());

  this->actors_count = mc_model_checker->process().actors().size();

  this->other_num = -1;
  this->atomic_propositions = std::move(atomic_propositions);
}

static bool evaluate_label(xbt_automaton_exp_label_t l, std::vector<int> const& values)
{
  switch (l->type) {
  case xbt_automaton_exp_label::AUT_OR:
    return evaluate_label(l->u.or_and.left_exp, values)
      || evaluate_label(l->u.or_and.right_exp, values);
  case xbt_automaton_exp_label::AUT_AND:
    return evaluate_label(l->u.or_and.left_exp, values)
      && evaluate_label(l->u.or_and.right_exp, values);
  case xbt_automaton_exp_label::AUT_NOT:
    return not evaluate_label(l->u.exp_not, values);
  case xbt_automaton_exp_label::AUT_PREDICAT:{
      unsigned int cursor = 0;
      xbt_automaton_propositional_symbol_t p = nullptr;
      xbt_dynar_foreach(simgrid::mc::property_automaton->propositional_symbols, cursor, p) {
        if (std::strcmp(xbt_automaton_propositional_symbol_get_name(p), l->u.predicat) == 0)
          return values[cursor] != 0;
      }
      xbt_die("Missing predicate");
      break;
    }
  case xbt_automaton_exp_label::AUT_ONE:
    return true;
  default:
    xbt_die("Unexpected vaue for automaton");
  }
}

Pair::Pair(unsigned long expanded_pairs) : num(expanded_pairs)
{}

std::shared_ptr<const std::vector<int>> LivenessChecker::get_proposition_values()
{
  std::vector<int> values;
  unsigned int cursor = 0;
  xbt_automaton_propositional_symbol_t ps = nullptr;
  xbt_dynar_foreach(simgrid::mc::property_automaton->propositional_symbols, cursor, ps)
    values.push_back(xbt_automaton_propositional_symbol_evaluate(ps));
  return std::make_shared<const std::vector<int>>(std::move(values));
}

std::shared_ptr<VisitedPair> LivenessChecker::insert_acceptance_pair(simgrid::mc::Pair* pair)
{
  std::shared_ptr<VisitedPair> new_pair = std::make_shared<VisitedPair>(
    pair->num, pair->automaton_state, pair->atomic_propositions,
    pair->graph_state);

  auto res = boost::range::equal_range(acceptance_pairs_, new_pair.get(),
                                       simgrid::mc::DerefAndCompareByActorsCountAndUsedHeap());

  if (pair->search_cycle) for (auto i = res.first; i != res.second; ++i) {
    std::shared_ptr<simgrid::mc::VisitedPair> const& pair_test = *i;
    if (xbt_automaton_state_compare(pair_test->automaton_state, new_pair->automaton_state) != 0 ||
        *(pair_test->atomic_propositions) != *(new_pair->atomic_propositions) ||
        not snapshot_equal(pair_test->graph_state->system_state.get(), new_pair->graph_state->system_state.get()))
      continue;
    XBT_INFO("Pair %d already reached (equal to pair %d) !", new_pair->num, pair_test->num);
    exploration_stack_.pop_back();
    if (dot_output != nullptr)
      fprintf(dot_output, "\"%d\" -> \"%d\" [%s];\n", this->previous_pair_, pair_test->num,
              this->previous_request_.c_str());
    return nullptr;
  }

  acceptance_pairs_.insert(res.first, new_pair);
  return new_pair;
}

void LivenessChecker::remove_acceptance_pair(int pair_num)
{
  for (auto i = acceptance_pairs_.begin(); i != acceptance_pairs_.end(); ++i)
    if ((*i)->num == pair_num) {
      acceptance_pairs_.erase(i);
      break;
    }
}

void LivenessChecker::replay()
{
  XBT_DEBUG("**** Begin Replay ****");

  /* Intermediate backtracking */
  if(_sg_mc_checkpoint > 0) {
    simgrid::mc::Pair* pair = exploration_stack_.back().get();
    if(pair->graph_state->system_state){
      pair->graph_state->system_state->restore(&mc_model_checker->process());
      return;
    }
  }

  /* Restore the initial state */
  simgrid::mc::session->restore_initial_state();

  /* Traverse the stack from the initial state and re-execute the transitions */
  int depth = 1;
  for (std::shared_ptr<Pair> const& pair : exploration_stack_) {
    if (pair == exploration_stack_.back())
      break;

    std::shared_ptr<State> state = pair->graph_state;

    if (pair->exploration_started) {

      int req_num             = state->transition_.argument_;
      smx_simcall_t saved_req = &state->executed_req_;

      smx_simcall_t req = nullptr;

      if (saved_req != nullptr) {
        /* because we got a copy of the executed request, we have to fetch the
             real one, pointed by the request field of the issuer process */
        const smx_actor_t issuer = MC_smx_simcall_get_issuer(saved_req);
        req = &issuer->simcall;

        /* Debug information */
        XBT_DEBUG("Replay (depth = %d) : %s (%p)",
          depth,
          simgrid::mc::request_to_string(
            req, req_num, simgrid::mc::RequestType::simix).c_str(),
          state.get());
      }

      this->get_session().execute(state->transition_);
    }

    /* Update statistics */
    visited_pairs_count_++;
    mc_model_checker->executed_transitions++;

    depth++;

  }

  XBT_DEBUG("**** End Replay ****");
}

/**
 * @brief Checks whether a given pair has already been visited by the algorithm.
 */
int LivenessChecker::insert_visited_pair(std::shared_ptr<VisitedPair> visited_pair, simgrid::mc::Pair* pair)
{
  if (_sg_mc_max_visited_states == 0)
    return -1;

  if (visited_pair == nullptr)
    visited_pair =
        std::make_shared<VisitedPair>(pair->num, pair->automaton_state, pair->atomic_propositions, pair->graph_state);

  auto range = boost::range::equal_range(visited_pairs_, visited_pair.get(),
                                         simgrid::mc::DerefAndCompareByActorsCountAndUsedHeap());

  for (auto i = range.first; i != range.second; ++i) {
    VisitedPair* pair_test = i->get();
    if (xbt_automaton_state_compare(pair_test->automaton_state, visited_pair->automaton_state) != 0 ||
        *(pair_test->atomic_propositions) != *(visited_pair->atomic_propositions) ||
        not snapshot_equal(pair_test->graph_state->system_state.get(), visited_pair->graph_state->system_state.get()))
      continue;
    if (pair_test->other_num == -1)
      visited_pair->other_num = pair_test->num;
    else
      visited_pair->other_num = pair_test->other_num;
    if (dot_output == nullptr)
      XBT_DEBUG("Pair %d already visited ! (equal to pair %d)", visited_pair->num, pair_test->num);
    else
      XBT_DEBUG("Pair %d already visited ! (equal to pair %d (pair %d in dot_output))",
        visited_pair->num, pair_test->num, visited_pair->other_num);
    (*i) = std::move(visited_pair);
    return (*i)->other_num;
  }

  visited_pairs_.insert(range.first, std::move(visited_pair));
  this->purge_visited_pairs();
  return -1;
}

void LivenessChecker::purge_visited_pairs()
{
  if (_sg_mc_max_visited_states != 0 && visited_pairs_.size() > (std::size_t)_sg_mc_max_visited_states) {
    // Remove the oldest entry with a linear search:
    visited_pairs_.erase(
        boost::min_element(visited_pairs_, [](std::shared_ptr<VisitedPair> const a,
                                              std::shared_ptr<VisitedPair> const& b) { return a->num < b->num; }));
  }
}

LivenessChecker::LivenessChecker(Session& s) : Checker(s)
{
}

RecordTrace LivenessChecker::get_record_trace() // override
{
  RecordTrace res;
  for (std::shared_ptr<Pair> const& pair : exploration_stack_)
    res.push_back(pair->graph_state->get_transition());
  return res;
}

void LivenessChecker::log_state() // override
{
  XBT_INFO("Expanded pairs = %lu", expanded_pairs_count_);
  XBT_INFO("Visited pairs = %lu", visited_pairs_count_);
  XBT_INFO("Executed transitions = %lu", mc_model_checker->executed_transitions);
}

void LivenessChecker::show_acceptance_cycle(std::size_t depth)
{
  XBT_INFO("*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*");
  XBT_INFO("|             ACCEPTANCE CYCLE            |");
  XBT_INFO("*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*");
  XBT_INFO("Counter-example that violates formula:");
  for (auto const& s : this->get_textual_trace())
    XBT_INFO("  %s", s.c_str());
  simgrid::mc::dumpRecordPath();
  simgrid::mc::session->log_state();
  XBT_INFO("Counter-example depth: %zu", depth);
}

std::vector<std::string> LivenessChecker::get_textual_trace() // override
{
  std::vector<std::string> trace;
  for (std::shared_ptr<Pair> const& pair : exploration_stack_) {
    int req_num       = pair->graph_state->transition_.argument_;
    smx_simcall_t req = &pair->graph_state->executed_req_;
    if (req && req->call_ != SIMCALL_NONE)
      trace.push_back(simgrid::mc::request_to_string(
        req, req_num, simgrid::mc::RequestType::executed));
  }
  return trace;
}

std::shared_ptr<Pair> LivenessChecker::create_pair(Pair* current_pair, xbt_automaton_state_t state,
                                                   std::shared_ptr<const std::vector<int>> propositions)
{
  expanded_pairs_count_++;
  std::shared_ptr<Pair> next_pair = std::make_shared<Pair>(expanded_pairs_count_);
  next_pair->automaton_state      = state;
  next_pair->graph_state = std::shared_ptr<simgrid::mc::State>(new simgrid::mc::State(++expanded_states_count_));
  next_pair->atomic_propositions  = std::move(propositions);
  if (current_pair)
    next_pair->depth = current_pair->depth + 1;
  else
    next_pair->depth = 1;
  /* Get enabled actors and insert them in the interleave set of the next graph_state */
  for (auto& actor : mc_model_checker->process().actors())
    if (simgrid::mc::actor_is_enabled(actor.copy.get_buffer()))
      next_pair->graph_state->add_interleaving_set(actor.copy.get_buffer());
  next_pair->requests = next_pair->graph_state->interleave_size();
  /* FIXME : get search_cycle value for each accepting state */
  if (next_pair->automaton_state->type == 1 || (current_pair && current_pair->search_cycle))
    next_pair->search_cycle = true;
  else
    next_pair->search_cycle = false;
  return next_pair;
}

void LivenessChecker::backtrack()
{
  /* Traverse the stack backwards until a pair with a non empty interleave
     set is found, deleting all the pairs that have it empty in the way. */
  while (not exploration_stack_.empty()) {
    std::shared_ptr<simgrid::mc::Pair> current_pair = exploration_stack_.back();
    exploration_stack_.pop_back();
    if (current_pair->requests > 0) {
      /* We found a backtracking point */
      XBT_DEBUG("Backtracking to depth %d", current_pair->depth);
      exploration_stack_.push_back(std::move(current_pair));
      this->replay();
      XBT_DEBUG("Backtracking done");
      break;
    } else {
      XBT_DEBUG("Delete pair %d at depth %d", current_pair->num, current_pair->depth);
      if (current_pair->automaton_state->type == 1)
        this->remove_acceptance_pair(current_pair->num);
    }
  }
}

void LivenessChecker::run()
{
  XBT_INFO("Check the liveness property %s", _sg_mc_property_file.get().c_str());
  MC_automaton_load(_sg_mc_property_file.get().c_str());

  XBT_DEBUG("Starting the liveness algorithm");
  simgrid::mc::session->initialize();

  /* Initialize */
  this->previous_pair_ = 0;

  std::shared_ptr<const std::vector<int>> propos = this->get_proposition_values();

  // For each initial state of the property automaton, push a
  // (application_state, automaton_state) pair to the exploration stack:
  unsigned int cursor = 0;
  xbt_automaton_state_t automaton_state;
  xbt_dynar_foreach (simgrid::mc::property_automaton->states, cursor, automaton_state)
    if (automaton_state->type == -1)
      exploration_stack_.push_back(this->create_pair(nullptr, automaton_state, propos));

  /* Actually run the double DFS search for counter-examples */
  while (not exploration_stack_.empty()) {
    std::shared_ptr<Pair> current_pair = exploration_stack_.back();

    /* Update current state in buchi automaton */
    simgrid::mc::property_automaton->current_state = current_pair->automaton_state;

    XBT_DEBUG(
        "********************* ( Depth = %d, search_cycle = %d, interleave size = %zu, pair_num = %d, requests = %d)",
        current_pair->depth, current_pair->search_cycle, current_pair->graph_state->interleave_size(),
        current_pair->num, current_pair->requests);

    if (current_pair->requests == 0) {
      this->backtrack();
      continue;
    }

    std::shared_ptr<VisitedPair> reached_pair;
    if (current_pair->automaton_state->type == 1 && not current_pair->exploration_started) {
      reached_pair = this->insert_acceptance_pair(current_pair.get());
      if (reached_pair == nullptr) {
        this->show_acceptance_cycle(current_pair->depth);
        throw simgrid::mc::LivenessError();
      }
    }

    /* Pair already visited ? stop the exploration on the current path */
    if (not current_pair->exploration_started) {
      int visited_num = this->insert_visited_pair(reached_pair, current_pair.get());
      if (visited_num != -1) {
        if (dot_output != nullptr) {
          fprintf(dot_output, "\"%d\" -> \"%d\" [%s];\n", this->previous_pair_, visited_num,
                  this->previous_request_.c_str());
          fflush(dot_output);
        }
        XBT_DEBUG("Pair already visited (equal to pair %d), exploration on the current path stopped.", visited_num);
        current_pair->requests = 0;
        this->backtrack();
        continue;
      }
    }

    smx_simcall_t req = MC_state_get_request(current_pair->graph_state.get());
    int req_num       = current_pair->graph_state->transition_.argument_;

    if (dot_output != nullptr) {
      if (this->previous_pair_ != 0 && this->previous_pair_ != current_pair->num) {
        fprintf(dot_output, "\"%d\" -> \"%d\" [%s];\n", this->previous_pair_, current_pair->num,
                this->previous_request_.c_str());
        this->previous_request_.clear();
      }
      this->previous_pair_    = current_pair->num;
      this->previous_request_ = simgrid::mc::request_get_dot_output(req, req_num);
      if (current_pair->search_cycle)
        fprintf(dot_output, "%d [shape=doublecircle];\n", current_pair->num);
      fflush(dot_output);
    }

    XBT_DEBUG("Execute: %s",
      simgrid::mc::request_to_string(
        req, req_num, simgrid::mc::RequestType::simix).c_str());

    /* Update stats */
    mc_model_checker->executed_transitions++;
    if (not current_pair->exploration_started)
      visited_pairs_count_++;

    /* Answer the request */
    mc_model_checker->handle_simcall(current_pair->graph_state->transition_);

    /* Wait for requests (schedules processes) */
    mc_model_checker->wait_for_requests();

    current_pair->requests--;
    current_pair->exploration_started = true;

    /* Get values of atomic propositions (variables used in the property formula) */
    std::shared_ptr<const std::vector<int>> prop_values = this->get_proposition_values();

    // For each enabled transition in the property automaton, push a
    // (application_state, automaton_state) pair to the exploration stack:
    for (int i = xbt_dynar_length(current_pair->automaton_state->out) - 1; i >= 0; i--) {
      xbt_automaton_transition_t transition_succ = (xbt_automaton_transition_t)xbt_dynar_get_as(
          current_pair->automaton_state->out, i, xbt_automaton_transition_t);
      if (evaluate_label(transition_succ->label, *prop_values))
        exploration_stack_.push_back(this->create_pair(current_pair.get(), transition_succ->dst, prop_values));
     }

  }

  XBT_INFO("No property violation found.");
  simgrid::mc::session->log_state();
}

Checker* createLivenessChecker(Session& s)
{
  return new LivenessChecker(s);
}

}
}
