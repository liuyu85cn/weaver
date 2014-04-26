/*
 * ===============================================================
 *    Description:  Vector timestamper server loop and request
 *                  processing methods.
 *
 *        Created:  07/22/2013 02:42:28 PM
 *
 *         Author:  Ayush Dubey, dubey@cs.cornell.edu
 *
 * Copyright (C) 2013, Cornell University, see the LICENSE file
 *                     for licensing agreement
 * ===============================================================
 */

#include <iostream>
#include <thread>
#include <vector>
#include <stdlib.h>
#include <signal.h>
#include <sys/time.h>

#define weaver_debug_
#include "common/vclock.h"
#include "common/transaction.h"
#include "common/event_order.h"
#include "node_prog/node_prog_type.h"
#include "node_prog/node_program.h"
#include "timestamper.h"

using coordinator::current_prog;
using coordinator::current_tx;
static coordinator::timestamper *vts;
static uint64_t vt_id;

namespace order
{
    chronos_client *kronos_cl = chronos_client_create(KRONOS_IPADDR, KRONOS_PORT);
    po6::threads::mutex kronos_mutex;
    std::list<uint64_t> *call_times = new std::list<uint64_t>();
    uint64_t cache_hits = 0;
    kronos_cache kcache;
}

// SIGINT handler
void
end_program(int signum)
{
    std::cerr << "Ending program, signum = " << signum << std::endl;
    exit(0);
}

// expects an input of list of writes that are part of this transaction
// for all writes, node mapper lookups should have already been performed
// for create requests, instead of lookup an entry for new handle should have been inserted
inline void
begin_transaction(transaction::pending_tx &tx)
{
    std::vector<transaction::pending_tx> tx_vec(NUM_SHARDS, transaction::pending_tx());

    vts->clk_mutex.lock();
    for (std::shared_ptr<transaction::pending_update> upd: tx.writes) {
        vts->qts.at(upd->loc1-SHARD_ID_INCR)++;
        upd->qts = vts->qts;
        tx_vec[upd->loc1-SHARD_ID_INCR].writes.emplace_back(upd);
    }
    vts->vclk.increment_clock();
    tx.timestamp = vts->vclk;
    // get unique tx id
    vts->clk_mutex.unlock();

    current_tx cur_tx(tx.client_id);
    for (uint64_t i = 0; i < NUM_SHARDS; i++) {
        if (!tx_vec[i].writes.empty()) {
            cur_tx.count++;
        }
    }
    // record txs as outstanding for reply bookkeeping and fault tolerance
    vts->tx_prog_mutex.lock();
    vts->outstanding_tx.emplace(tx.id, cur_tx);
    vts->tx_prog_mutex.unlock();

    // send tx batches
    message::message msg;
    for (uint64_t i = 0; i < NUM_SHARDS; i++) {
        if (!tx_vec[i].writes.empty()) {
            tx_vec[i].timestamp = tx.timestamp;
            tx_vec[i].id = tx.id;
            message::prepare_message(msg, message::TX_INIT, vt_id, tx.timestamp, tx_vec[i].writes.at(0)->qts, tx.id, tx_vec[i].writes);
            vts->comm.send(tx_vec[i].writes.at(0)->loc1, msg.buf);
        }
    }
}

// decrement reply count. if all replies have been received, ack to client
inline void
end_transaction(uint64_t tx_id, int thread_id)
{
    vts->tx_prog_mutex.lock();
    if (--vts->outstanding_tx.at(tx_id).count == 0) {
        // done tx
        //vts->hstub[thread_id]->del_tx(tx_id);
        uint64_t client_id = vts->outstanding_tx[tx_id].client;
        vts->outstanding_tx.erase(tx_id);
        vts->tx_prog_mutex.unlock();
        message::message msg;
        message::prepare_message(msg, message::CLIENT_TX_DONE);
        vts->comm.send(client_id, msg.buf);
    } else {
        vts->tx_prog_mutex.unlock();
    }
}

// single dedicated thread which wakes up after given timeout, sends updates, and sleeps
inline void
timer_function()
{
    timespec sleep_time;
    int sleep_ret;
    int sleep_flags = 0;
    vc::vclock vclk(vt_id, 0);
    vc::qtimestamp_t qts;
    uint64_t req_id, max_done_id;
    vc::vclock_t max_done_clk;
    uint64_t num_outstanding_progs;
    typedef std::vector<std::pair<uint64_t, node_prog::prog_type>> done_req_t;
    std::vector<done_req_t> done_reqs(NUM_SHARDS, done_req_t());
    std::vector<uint64_t> del_done_reqs;
    message::message msg;
    bool nop_sent, clock_synced;

    sleep_time.tv_sec  = VT_TIMEOUT_NANO / NANO;
    sleep_time.tv_nsec = VT_TIMEOUT_NANO % NANO;

    while (true) {
        sleep_ret = clock_nanosleep(CLOCK_REALTIME, sleep_flags, &sleep_time, NULL);
        if (sleep_ret != 0 && sleep_ret != EINTR) {
            assert(false);
        }
        nop_sent = false;
        clock_synced = false;
        vts->periodic_update_mutex.lock();
        
        // send nops and state cleanup info to shards
        if (vts->to_nop.any()) {
            req_id = vts->generate_id();
            vts->clk_mutex.lock();
            vts->vclk.increment_clock();
            vclk.clock = vts->vclk.clock;
            for (uint64_t shard_id = 0; shard_id < NUM_SHARDS; shard_id++) {
                if (vts->to_nop[shard_id]) {
                    vts->qts[shard_id]++;
                    done_reqs[shard_id].clear();
                }
            }
            qts = vts->qts;
            vts->clk_mutex.unlock();

            del_done_reqs.clear();
            vts->tx_prog_mutex.lock();
            max_done_id = vts->max_done_id;
            max_done_clk = *vts->max_done_clk;
            num_outstanding_progs = vts->pend_prog_queue.size();
            for (auto &x: vts->done_reqs) {
                // x.first = node prog type
                // x.second = unordered_map <req_id -> bitset<NUM_SHARDS>>
                for (auto &reply: x.second) {
                    // reply.first = req_id
                    // reply.second = bitset<NUM_SHARDS>
                    for (uint64_t shard_id = 0; shard_id < NUM_SHARDS; shard_id++) {
                        if (vts->to_nop[shard_id] && !reply.second[shard_id]) {
                            reply.second.set(shard_id);
                            done_reqs[shard_id].emplace_back(std::make_pair(reply.first, x.first));
                        }
                    }
                    if (reply.second.all()) {
                        del_done_reqs.emplace_back(reply.first);
                    }
                }
                for (auto &del: del_done_reqs) {
                    x.second.erase(del);
                }
            }
            vts->tx_prog_mutex.unlock();

            for (uint64_t shard_id = 0; shard_id < NUM_SHARDS; shard_id++) {
                if (vts->to_nop[shard_id]) {
                    assert(vclk.clock.size() == NUM_VTS);
                    assert(max_done_clk.size() == NUM_VTS);
                    message::prepare_message(msg, message::VT_NOP, vt_id, vclk, qts, req_id,
                        done_reqs[shard_id], max_done_id, max_done_clk,
                        num_outstanding_progs, vts->shard_node_count);
                    vts->comm.send(shard_id + SHARD_ID_INCR, msg.buf);
                }
            }
            vts->to_nop.reset();
            nop_sent = true;
        }

        // update vclock at other timestampers
        if (vts->clock_update_acks == (NUM_VTS-1) && NUM_VTS > 1) {
            clock_synced = true;
            vts->clock_update_acks = 0;
            if (!nop_sent) {
                vts->clk_mutex.lock();
                vclk.clock = vts->vclk.clock;
                vts->clk_mutex.unlock();
            }
            for (uint64_t i = 0; i < NUM_VTS; i++) {
                if (i == vt_id) {
                    continue;
                }
                message::prepare_message(msg, message::VT_CLOCK_UPDATE, vt_id, vclk.clock[vt_id]);
                vts->comm.send(i, msg.buf);
            }
        }

        if (nop_sent && !clock_synced) {
        //    WDEBUG << "nop yes, clock no" << std::endl;
        } else if (!nop_sent && clock_synced) {
        //    WDEBUG << "clock yes, nop no" << std::endl;
        }

        vts->periodic_update_mutex.unlock();
    }
}

// unpack client message for a node program, prepare shard msges, and send out
template <typename ParamsType, typename NodeStateType, typename CacheValueType>
void node_prog :: particular_node_program<ParamsType, NodeStateType, CacheValueType> :: 
    unpack_and_start_coord(std::unique_ptr<message::message> msg, uint64_t clientID, int thread_id)
{
    node_prog::prog_type pType;
    std::vector<std::pair<uint64_t, ParamsType>> initial_args;

    message::unpack_message(*msg, message::CLIENT_NODE_PROG_REQ, pType, initial_args);
    
    // map from locations to a list of start_node_params to send to that shard
    std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, ParamsType>>> initial_batches; 
    bool global_req = false;

    // lookup mappings
    std::unordered_map<uint64_t, uint64_t> request_element_mappings;
    std::unordered_set<uint64_t> mappings_to_get;
    for (auto &initial_arg : initial_args) {
        uint64_t c_id = initial_arg.first;
        if (c_id == -1) { // max uint64_t means its a global thing like triangle count
            assert(mappings_to_get.empty()); // dont mix global req with normal nodes
            assert(initial_args.size() == 1);
            global_req = true;
            break;
        }
        mappings_to_get.insert(c_id);
    }
    if (!mappings_to_get.empty()) {
        auto results = vts->nmap_client[thread_id]->get_mappings(mappings_to_get);
        assert(results.size() == mappings_to_get.size());
        for (auto &toAdd : results) {
            request_element_mappings.emplace(toAdd);
        }
    }

    if (global_req) {
        // send copy of params to each shard
        for (int i = 0; i < NUM_SHARDS; i++) {
            initial_batches[i + SHARD_ID_INCR].emplace_back(std::make_pair(initial_args[0].first,
                    initial_args[0].second));
        }
    } else { // regular style node program
        for (std::pair<uint64_t, ParamsType> &node_params_pair: initial_args) {
            uint64_t loc = request_element_mappings[node_params_pair.first];
            initial_batches[loc].emplace_back(std::make_pair(node_params_pair.first,
                    std::move(node_params_pair.second)));
        }
    }
    
    vts->clk_mutex.lock();
    vts->vclk.increment_clock();
    vc::vclock req_timestamp = vts->vclk;
    assert(req_timestamp.clock.size() == NUM_VTS);
    vts->clk_mutex.unlock();

    /*
    if (global_req) {
    }
    */
    vts->tx_prog_mutex.lock();
    uint64_t req_id = vts->generate_id();
    vts->outstanding_progs.emplace(req_id, current_prog(clientID, req_timestamp.clock));
    vts->pend_prog_queue.emplace(req_id);
    vts->tx_prog_mutex.unlock();

    message::message msg_to_send;
    for (auto &batch_pair : initial_batches) {
        message::prepare_message(msg_to_send, message::NODE_PROG, pType, global_req, vt_id, req_timestamp, req_id, vt_id, batch_pair.second);
        vts->comm.send(batch_pair.first, msg_to_send.buf);
    }
}

template <typename ParamsType, typename NodeStateType, typename CacheValueType>
void node_prog :: particular_node_program<ParamsType, NodeStateType, CacheValueType> ::
    unpack_and_run_db(std::unique_ptr<message::message>)
{ }

template <typename ParamsType, typename NodeStateType, typename CacheValueType>
void node_prog :: particular_node_program<ParamsType, NodeStateType, CacheValueType> ::
    unpack_context_reply_db(std::unique_ptr<message::message>)
{ }

// remove a completed node program from outstanding requests data structure
// update 'max_done_id' and 'max_done_clk' accordingly
// caution: need to hold vts->tx_prog_mutex
inline void
mark_req_finished(uint64_t req_id)
{
    assert(vts->seen_done_id.find(req_id) == vts->seen_done_id.end());
    vts->seen_done_id.emplace(req_id);
    if (vts->pend_prog_queue.top() == req_id) {
        assert(vts->max_done_id < vts->pend_prog_queue.top());
        vts->max_done_id = req_id;
        //WDEBUG << "max_done_id set to " << req_id << std::endl;
        assert(vts->outstanding_progs.find(vts->max_done_id) != vts->outstanding_progs.end());
        vts->max_done_clk = std::move(vts->outstanding_progs[vts->max_done_id].vclk);
        vts->pend_prog_queue.pop();
        vts->outstanding_progs.erase(vts->max_done_id);
        while (!vts->pend_prog_queue.empty() && !vts->done_prog_queue.empty()
            && vts->pend_prog_queue.top() == vts->done_prog_queue.top()) {
            assert(vts->max_done_id < vts->pend_prog_queue.top());
            vts->max_done_id = vts->pend_prog_queue.top();
            //WDEBUG << "max_done_id set to " << vts->pend_prog_queue.top() << std::endl;
            assert(vts->outstanding_progs.find(vts->max_done_id) != vts->outstanding_progs.end());
            vts->max_done_clk = std::move(vts->outstanding_progs[vts->max_done_id].vclk);
            vts->pend_prog_queue.pop();
            vts->done_prog_queue.pop();
            vts->outstanding_progs.erase(vts->max_done_id);
        }
    } else {
        vts->done_prog_queue.emplace(req_id);
    }
}

void
server_loop(int thread_id)
{
    busybee_returncode ret;
    uint32_t code;
    enum message::msg_type mtype;
    std::unique_ptr<message::message> msg;
    uint64_t sender, tx_id;
    node_prog::prog_type pType;

    while (true) {
        msg.reset(new message::message());
        ret = vts->comm.recv(&sender, &msg->buf);
        if (ret != BUSYBEE_SUCCESS) {
            continue;
        } else {
            // good to go, unpack msg
            uint64_t _size;
            msg->buf->unpack_from(BUSYBEE_HEADER_SIZE) >> code >> _size;
            mtype = (enum message::msg_type)code;
            sender -= ID_INCR;

            switch (mtype) {
                // client messages
                case message::CLIENT_TX_INIT: {
                    transaction::pending_tx tx;
                    if (!vts->unpack_tx(*msg, tx, sender, thread_id)) {
                        message::prepare_message(*msg, message::CLIENT_TX_FAIL);
                        vts->comm.send(sender, msg->buf);
                    } else {
                        begin_transaction(tx);
                    }
                    break;
                }

                case message::VT_CLOCK_UPDATE: {
                    uint64_t rec_vtid, rec_clock;
                    message::unpack_message(*msg, message::VT_CLOCK_UPDATE, rec_vtid, rec_clock);
                    vts->clk_mutex.lock();
                    vts->vclk.update_clock(rec_vtid, rec_clock);
                    vts->clk_mutex.unlock();
                    message::prepare_message(*msg, message::VT_CLOCK_UPDATE_ACK);
                    vts->comm.send(rec_vtid, msg->buf);
                    break;
                }

                case message::VT_CLOCK_UPDATE_ACK:
                    vts->periodic_update_mutex.lock();
                    vts->clock_update_acks++;
                    assert(vts->clock_update_acks < NUM_VTS);
                    vts->periodic_update_mutex.unlock();
                    break;

                case message::VT_NOP_ACK: {
                    uint64_t shard_node_count;
                    message::unpack_message(*msg, message::VT_NOP_ACK, sender, shard_node_count);
                    vts->periodic_update_mutex.lock();
                    vts->shard_node_count[sender - SHARD_ID_INCR] = shard_node_count;

                    vts->to_nop.set(sender - SHARD_ID_INCR);
                    vts->periodic_update_mutex.unlock();
                    break;
                }

                case message::CLIENT_MSG_COUNT: {
                    vts->msg_count_mutex.lock();
                    vts->msg_count = 0;
                    vts->msg_count_acks = 0;
                    vts->msg_count_mutex.unlock();
                    for (uint64_t i = SHARD_ID_INCR; i < (SHARD_ID_INCR + NUM_SHARDS); i++) {
                        message::prepare_message(*msg, message::MSG_COUNT, vt_id);
                        vts->comm.send(i, msg->buf);
                    }
                    break;
                }

                case message::CLIENT_NODE_COUNT: {
                    vts->periodic_update_mutex.lock();
                    message::prepare_message(*msg, message::NODE_COUNT_REPLY, vts->shard_node_count);
                    vts->periodic_update_mutex.unlock();
                    vts->comm.send(sender, msg->buf);
                    break;
                }

                // shard messages
                case message::LOADED_GRAPH: {
                    uint64_t load_time;
                    message::unpack_message(*msg, message::LOADED_GRAPH, load_time);
                    vts->graph_load_mutex.lock();
                    if (load_time > vts->max_load_time) {
                        vts->max_load_time = load_time;
                    }
                    if (++vts->load_count == NUM_SHARDS) {
                        WDEBUG << "Graph loaded on all machines, time taken = " << vts->max_load_time << " nanosecs." << std::endl;
                    }
                    vts->graph_load_mutex.unlock();
                    break;
                }

                case message::TX_DONE:
                    message::unpack_message(*msg, message::TX_DONE, tx_id);
                    end_transaction(tx_id, thread_id);
                    break;

                case message::START_MIGR: {
                    uint64_t hops = MAX_UINT64;
                    message::prepare_message(*msg, message::MIGRATION_TOKEN, hops, vt_id);
                    vts->comm.send(START_MIGR_ID, msg->buf); 
                    break;
                }

                case message::ONE_STREAM_MIGR: {
                    uint64_t hops = NUM_SHARDS;
                    vts->migr_mutex.lock();
                    vts->migr_client = sender;
                    vts->migr_mutex.unlock();
                    message::prepare_message(*msg, message::MIGRATION_TOKEN, hops, vt_id);
                    vts->comm.send(START_MIGR_ID, msg->buf);
                    break;
                }

                case message::MIGRATION_TOKEN: {
                    vts->migr_mutex.lock();
                    uint64_t client = vts->migr_client;
                    vts->migr_mutex.unlock();
                    message::prepare_message(*msg, message::DONE_MIGR);
                    vts->comm.send(client, msg->buf);
                    WDEBUG << "Shard node counts are:";
                    for (uint64_t &x: vts->shard_node_count) {
                        std::cerr << " " << x;
                    }
                    std::cerr << std::endl;
                    break;
                }

                case message::CLIENT_NODE_PROG_REQ:
                    message::unpack_partial_message(*msg, message::CLIENT_NODE_PROG_REQ, pType);
                    node_prog::programs.at(pType)->unpack_and_start_coord(std::move(msg), sender, thread_id);
                    break;

                // node program response from a shard
                case message::NODE_PROG_RETURN:
                    uint64_t req_id;
                    node_prog::prog_type type;
                    message::unpack_partial_message(*msg, message::NODE_PROG_RETURN, type, req_id); // don't unpack rest
                    vts->tx_prog_mutex.lock();
                    if (vts->outstanding_progs.find(req_id) != vts->outstanding_progs.end()) { // TODO: change to .count (AD: why?)
                        uint64_t client = vts->outstanding_progs[req_id].client;
                        /*
                        if (vts->outstanding_triangle_progs.count(req_id) > 0) { // a triangle prog response
                            std::pair<int, node_prog::triangle_params>& p = vts->outstanding_triangle_progs.at(req_id);
                            p.first--; // count of shards responded

                            // unpack whole thing
                            std::pair<uint64_t, node_prog::triangle_params> tempPair;
                            message::unpack_message(*msg, message::NODE_PROG_RETURN, type, req_id, tempPair);

                            uint64_t oldval = p.second.num_edges;
                            p.second.num_edges += tempPair.second.num_edges;

                            // XXX temp make sure reference worked (AD: let's fix this)
                            assert(vts->outstanding_triangle_progs.at(req_id).second.num_edges - tempPair.second.num_edges == oldval); 

                            if (p.first == 0) { // all shards responded
                                // send back to client
                                vts->done_reqs[type].emplace(req_id, std::bitset<NUM_SHARDS>());
                                tempPair.second.num_edges = p.second.num_edges;
                                message::prepare_message(*msg, message::NODE_PROG_RETURN, type, req_id, tempPair);
                                vts->comm.send(client_to_ret, msg->buf);
                                mark_req_finished(req_id);
                            }
                        } else {*/
                            // just a normal node program
                            vts->done_reqs[type].emplace(req_id, std::bitset<NUM_SHARDS>());
                            vts->comm.send(client, msg->buf);
                            mark_req_finished(req_id);
                        //}
                    } else {
                        WDEBUG << "node prog return for already completed or never existed req id" << std::endl;
                    }
                    vts->tx_prog_mutex.unlock();
                    break;

                case message::MSG_COUNT: {
                    uint64_t shard, msg_count;
                    message::unpack_message(*msg, message::MSG_COUNT, shard, msg_count);
                    vts->msg_count_mutex.lock();
                    vts->msg_count += msg_count;
                    if (++vts->msg_count_acks == NUM_SHARDS) {
                        WDEBUG << "Msg count = " << vts->msg_count << std::endl;
                    }
                    vts->msg_count_mutex.unlock();
                    break;
                }

                default:
                    std::cerr << "unexpected msg type " << mtype << std::endl;
            }
        }
    }
}

void
server_manager_link_loop(po6::net::hostname sm_host)
{
    // Most of the following code has been 'borrowed' from
    // Robert Escriva's HyperDex.
    // see https://github.com/rescrv/HyperDex for the original code.

    vts->sm_stub.set_server_manager_address(sm_host.address.c_str(), sm_host.port);

    if (!vts->sm_stub.register_id(vts->server, *vts->comm.get_loc()))
    {
        return;
    }

    bool cluster_jump = false;

    while (!vts->sm_stub.should_exit())
    {
        if (!vts->sm_stub.maintain_link())
        {
            continue;
        }
        const configuration& old_config(vts->config);
        const configuration& new_config(vts->sm_stub.config());

        if (old_config.cluster() != 0 &&
            old_config.cluster() != new_config.cluster())
        {
            cluster_jump = true;
            break;
        }

        if (old_config.version() > new_config.version())
        {
            WDEBUG << "received new configuration version=" << new_config.version()
                   << " that's older than our current configuration version="
                   << old_config.version();
            continue;
        }
        // if old_config.version == new_config.version, still fetch

        vts->config_mutex.lock();
        vts->config = new_config;
        if (!vts->first_config) {
            vts->first_config = true;
            vts->first_config_cond.signal();
        } else {
            vts->reconfigure();
        }
        vts->config_mutex.unlock();

        // let the coordinator know we've moved to this config
        vts->sm_stub.config_ack(new_config.version());
    }

    if (cluster_jump)
    {
        WDEBUG << "\n================================================================================\n"
               << "Exiting because the server manager changed on us.\n"
               << "This is most likely an operations error."
               << "================================================================================";
    }
    else if (vts->sm_stub.should_exit() && !vts->sm_stub.config().exists(vts->server))
    {
        WDEBUG << "\n================================================================================\n"
               << "Exiting because the server manager says it doesn't know about this node.\n"
               << "================================================================================";
    }
}

void
install_signal_handler(int signum, void (*handler)(int))
{
    struct sigaction sa;
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    int ret = sigaction(signum, &sa, NULL);
    assert(ret == 0);
}

int
main(int argc, char *argv[])
{
    if (argc < 2 || argc > 3) {
        WDEBUG << "Usage,   primary vt:" << argv[0] << " <vector_timestamper_id>" << std::endl
               << "          backup vt:" << argv[0] << " <vector_timestamper_id> <backup_number>" << std::endl; 
        return -1;
    }

    install_signal_handler(SIGINT, end_program);
    install_signal_handler(SIGHUP, end_program);
    install_signal_handler(SIGTERM, end_program);

    // vt setup
    vt_id = atoi(argv[1]);
    if (argc == 3) {
        vts = new coordinator::timestamper(vt_id, atoi(argv[2]));
        assert((atoi(argv[2]) - vt_id) % (NUM_VTS+NUM_SHARDS) == 0);
    } else {
        vts = new coordinator::timestamper(vt_id, vt_id);
    }

    // server manager link
    std::thread sm_thr(server_manager_link_loop,
        po6::net::hostname(SERVER_MANAGER_IPADDR, SERVER_MANAGER_PORT));
    sm_thr.detach();

    vts->config_mutex.lock();

    // wait for first config to arrive from server manager
    while (!vts->first_config) {
        vts->first_config_cond.wait();
    }

    // registered this server with server_manager, config has fairly recent value
    vts->init();

    vts->config_mutex.unlock();

    // start all threads
    std::thread *thr;
    for (int i = 0; i < NUM_THREADS; i++) {
        thr = new std::thread(server_loop, i);
        thr->detach();
    }

    if (argc == 3) {
        // wait till this server becomes primary vt
        vts->config_mutex.lock();
        while (!vts->active_backup) {
            vts->backup_cond.wait();
        }
        vts->config_mutex.unlock();
        WDEBUG << "backup " << atoi(argv[2]) << " now primary for vt " << vt_id << std::endl;
        vts->restore_backup();
    } else {
        // this server is primary vt, start now
        std::cout << "Vector timestamper " << vt_id << std::endl;
    }

    // initial wait for all vector timestampers to start
    // TODO change this to use config pushed by server manager
    timespec sleep_time;
    sleep_time.tv_sec =  INITIAL_TIMEOUT_NANO / NANO;
    sleep_time.tv_nsec = INITIAL_TIMEOUT_NANO % NANO;
    int ret = clock_nanosleep(CLOCK_REALTIME, 0, &sleep_time, NULL);
    assert(ret == 0);
    WDEBUG << "Initial setup delay complete" << std::endl;

    UNUSED(ret);

    // call periodic thread function
    timer_function();
}

#undef weaver_debug_
