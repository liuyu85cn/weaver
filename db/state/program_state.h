/*
 * ===============================================================
 *    Description:  State corresponding to a node program.
 *
 *        Created:  04/23/2013 10:44:00 AM
 *
 *         Author:  Ayush Dubey, dubey@cs.cornell.edu
 *
 * Copyright (C) 2013, Cornell University, see the LICENSE file
 *                     for licensing agreement
 * ===============================================================
 */

#ifndef weaver_db_state_program_state_h_
#define weaver_db_state_program_state_h_

#include <unordered_map>
#include <unordered_set>
#include <po6/threads/mutex.h>

#include "node_prog/node_prog_type.h"
#include "node_prog/reach_program.h"
//#include "node_prog/n_hop_reach_program.h"
//#include "node_prog/triangle_program.h"
#include "node_prog/dijkstra_program.h"
#include "node_prog/clustering_program.h"
#include "node_prog/read_node_props_program.h"
#include "node_prog/read_edges_props_program.h"
#include "node_prog/read_n_edges_program.h"
#include "common/message.h"

namespace state
{
    typedef std::unordered_map<uint64_t, std::shared_ptr<node_prog::Node_State_Base>> node_map;
    typedef std::unordered_map<uint64_t, std::shared_ptr<node_map>> req_map;
    typedef std::unordered_map<node_prog::prog_type, req_map> prog_map;
    // for permanent deletion
    typedef std::unordered_map<uint64_t, std::unordered_set<uint64_t>> node_to_reqs;

    class program_state
    {
        prog_map prog_state;
        node_to_reqs req_list;
        uint64_t completed_id; // all nodes progs with id < completed_id are done
        std::unordered_set<uint64_t> done_ids;
        po6::threads::mutex mutex;
        po6::threads::cond in_use_cond;
#ifdef weaver_debug_
        bool holding;
#endif

        public:
            program_state();

        public:
            bool state_exists(node_prog::prog_type t, uint64_t req_id, uint64_t node_id);
            std::shared_ptr<node_prog::Node_State_Base> get_state(node_prog::prog_type t,
                    uint64_t req_id, uint64_t node_id);
            void put_state(node_prog::prog_type t, uint64_t req_id, uint64_t node_id,
                    std::shared_ptr<node_prog::Node_State_Base> new_state);
            uint64_t size(uint64_t node_id);
            void pack(uint64_t node_id, e::buffer::packer &packer);
            void unpack(uint64_t node_id, e::unpacker &unpacker);
            void delete_node_state(uint64_t node_id);
            void done_requests(std::vector<std::pair<uint64_t, node_prog::prog_type>>&);
            bool check_done_request(uint64_t req_id);

        private:
            void acquire();
            void release();
            bool state_exists_nolock(node_prog::prog_type t, uint64_t req_id, uint64_t node_id);
            bool node_entry_exists_nolock(node_prog::prog_type t, uint64_t node_id);
            bool check_done_nolock(uint64_t req_id);
    };

    inline
    program_state :: program_state()
        : completed_id(0)
        , in_use_cond(&mutex)
#ifdef weaver_debug_
        , holding(false)
#endif
    {
        req_map new_req_map;
        prog_state.emplace(node_prog::REACHABILITY, new_req_map);
        prog_state.emplace(node_prog::N_HOP_REACHABILITY, new_req_map);
        prog_state.emplace(node_prog::TRIANGLE_COUNT, new_req_map);
        prog_state.emplace(node_prog::CLUSTERING, new_req_map);
        prog_state.emplace(node_prog::DIJKSTRA, new_req_map);
        prog_state.emplace(node_prog::READ_NODE_PROPS, new_req_map);
        prog_state.emplace(node_prog::READ_EDGES_PROPS, new_req_map);
    }

    inline void
    program_state :: acquire()
    {
        mutex.lock();
#ifdef weaver_debug_
        holding = true;
#endif
    }

    inline void
    program_state :: release()
    {
#ifdef weaver_debug_
        holding = false;
#endif
        mutex.unlock();
    }

    inline bool
    program_state :: state_exists_nolock(node_prog::prog_type t, uint64_t req_id, uint64_t node_id)
    {
        assert(prog_state.count(t) > 0); // make sure we can store this prog_type
        req_map &rmap = prog_state.at(t);
        req_map::iterator rmap_iter = rmap.find(req_id);
        if (rmap_iter == rmap.end()) {
            return false;
        }
        node_map &nmap = *rmap.at(req_id);
        node_map::iterator nmap_iter = nmap.find(node_id);
        if (nmap_iter == nmap.end()) {
            return false;
        } else {
            return true;
        }
    }
    
    inline bool
    program_state :: state_exists(node_prog::prog_type t, uint64_t req_id, uint64_t node_id)
    {
        bool exists;
        acquire();
        exists = state_exists_nolock(t, req_id, node_id);
        release();
        return exists;
    }

    // if state exists, return a shared_ptr to the state
    // else return a null shared_ptr
    inline std::shared_ptr<node_prog::Node_State_Base>
    program_state :: get_state(node_prog::prog_type t, uint64_t req_id, uint64_t node_id)
    {
        std::shared_ptr<node_prog::Node_State_Base> state;
        acquire();
        if (state_exists_nolock(t, req_id, node_id)) {
            state = prog_state.at(t).at(req_id)->at(node_id);
        }
        release();
        return state;
    }

    // insert new state in state map, unless request has already completed
    // if request completed, no-op
    inline void
    program_state :: put_state(node_prog::prog_type t, uint64_t req_id, uint64_t node_id,
        std::shared_ptr<node_prog::Node_State_Base> new_state)
    {
        acquire();
        if (!check_done_nolock(req_id)) { // check request not done yet
            req_map &rmap = prog_state.at(t);
            if (rmap.find(req_id) != rmap.end()) {
                (*prog_state.at(t).at(req_id))[node_id] = new_state;
            } else {
                std::shared_ptr<node_map> nmap(new node_map());
                nmap->emplace(node_id, new_state);
                prog_state.at(t).emplace(req_id, nmap);
            }
            if (req_list.find(node_id) == req_list.end()) {
                req_list.emplace(node_id, std::unordered_set<uint64_t>());
            }
            req_list.at(node_id).emplace(req_id);
        } else {
            WDEBUG << "not putting state, request " << req_id << " completed" << std::endl;
        }
        release();
    }
    
    inline uint64_t
    program_state :: size(uint64_t node_id)
    {
        uint64_t sz = 0;
        uint16_t ptype;
        uint64_t num_entries = 0;
        acquire();
        sz += message::size(num_entries);
        if (req_list.find(node_id) != req_list.end()) {
            // there is state corresponding to this node
            std::unordered_set<uint64_t> reqs = req_list.at(node_id);
            for (uint64_t req_id: reqs) {
                for (auto &t: prog_state) {
                    req_map &rmap = t.second;
                    if (rmap.find(req_id) != rmap.end()) {
                        sz += message::size(ptype) + message::size(req_id);
                        switch (t.first) {
                            case node_prog::REACHABILITY: {
                                std::shared_ptr<node_prog::reach_node_state> rns = 
                                    std::dynamic_pointer_cast<node_prog::reach_node_state>(rmap.at(req_id)->at(node_id));
                                sz += rns->size();
                                break;
                            }
                            /*
                            case node_prog::TRIANGLE_COUNT: {
                                std::shared_ptr<node_prog::triangle_node_state> rns = 
                                    std::dynamic_pointer_cast<node_prog::triangle_node_state>(rmap.at(req_id)->at(node_id));
                                sz += rns->size();
                                break;
                            }
                            case node_prog::N_HOP_REACHABILITY: {
                                std::shared_ptr<node_prog::n_hop_reach_node_state> rns = 
                                    std::dynamic_pointer_cast<node_prog::n_hop_reach_node_state>(rmap.at(req_id)->at(node_id));
                                sz += rns->size();
                                break;
                            }

*/
                            case node_prog::DIJKSTRA: {
                                std::shared_ptr<node_prog::dijkstra_node_state> dns =
                                    std::dynamic_pointer_cast<node_prog::dijkstra_node_state>(rmap.at(req_id)->at(node_id));
                                sz += dns->size();
                                break;
                            }
                            case node_prog::CLUSTERING: {
                                std::shared_ptr<node_prog::clustering_node_state> cns =
                                    std::dynamic_pointer_cast<node_prog::clustering_node_state>(rmap.at(req_id)->at(node_id));
                                sz += cns->size();
                                break;
                            }
                            case node_prog::READ_NODE_PROPS: {
                                std::shared_ptr<node_prog::read_node_props_state> cns =
                                    std::dynamic_pointer_cast<node_prog::read_node_props_state>(rmap.at(req_id)->at(node_id));
                                sz += cns->size();
                                break;
                            }
                            case node_prog::READ_EDGES_PROPS: {
                                std::shared_ptr<node_prog::read_edges_props_state> cns =
                                    std::dynamic_pointer_cast<node_prog::read_edges_props_state>(rmap.at(req_id)->at(node_id));
                                sz += cns->size();
                                break;
                            }
                            case node_prog::READ_N_EDGES: {
                                std::shared_ptr<node_prog::read_n_edges_state> cns =
                                    std::dynamic_pointer_cast<node_prog::read_n_edges_state>(rmap.at(req_id)->at(node_id));
                                sz += cns->size();
                                break;
                            }

                            default:
                                WDEBUG << "Bad type in program state size " << t.first << std::endl;
                        }
                        break;
                    }
                }
            }
        }
        release();
        return sz;
    }

    inline void
    program_state :: pack(uint64_t node_id, e::buffer::packer &packer)
    {
        uint16_t ptype;
        uint64_t num_entries = 0;
        acquire();
        if (req_list.find(node_id) != req_list.end()) {
            // there is state for this node
            std::unordered_set<uint64_t> reqs = req_list.at(node_id);
            num_entries = reqs.size();
            message::pack_buffer(packer, num_entries);
            for (uint64_t req_id: reqs) {
                for (auto &t: prog_state) {
                    req_map &rmap = t.second;
                    ptype = (uint16_t)t.first;
                    if (rmap.find(req_id) != rmap.end()) {
                        message::pack_buffer(packer, ptype);
                        message::pack_buffer(packer, req_id);
                        switch (t.first) {
                            case node_prog::REACHABILITY: {
                                std::shared_ptr<node_prog::reach_node_state> rns = 
                                    std::dynamic_pointer_cast<node_prog::reach_node_state>(rmap.at(req_id)->at(node_id));
                                rns->pack(packer);
                                break;
                            }

                            case node_prog::DIJKSTRA: {
                                std::shared_ptr<node_prog::dijkstra_node_state> dns =
                                    std::dynamic_pointer_cast<node_prog::dijkstra_node_state>(rmap.at(req_id)->at(node_id));
                                dns->pack(packer);
                                break;
                            }

                            case node_prog::CLUSTERING: {
                                std::shared_ptr<node_prog::clustering_node_state> cns =
                                    std::dynamic_pointer_cast<node_prog::clustering_node_state>(rmap.at(req_id)->at(node_id));
                                cns->pack(packer);
                                break;
                            }

                            case node_prog::READ_NODE_PROPS: {
                                std::shared_ptr<node_prog::read_node_props_state> cns =
                                    std::dynamic_pointer_cast<node_prog::read_node_props_state>(rmap.at(req_id)->at(node_id));
                                cns->pack(packer);
                                break;
                            }

                            case node_prog::READ_EDGES_PROPS: {
                                std::shared_ptr<node_prog::read_edges_props_state> cns =
                                    std::dynamic_pointer_cast<node_prog::read_edges_props_state>(rmap.at(req_id)->at(node_id));
                                cns->pack(packer);
                                break;
                            }

                            case node_prog::READ_N_EDGES: {
                                std::shared_ptr<node_prog::read_n_edges_state> cns =
                                    std::dynamic_pointer_cast<node_prog::read_n_edges_state>(rmap.at(req_id)->at(node_id));
                                cns->pack(packer);
                                break;
                            }

                            default:
                                WDEBUG << "Bad type in program state pack " << t.first << std::endl;
                        }
                        break;
                    }
                }
            }
        } else {
            message::pack_buffer(packer, num_entries);
        }
        release();
    }

    inline void
    program_state :: unpack(uint64_t node_id, e::unpacker &unpacker)
    {
        uint16_t ptype;
        uint64_t num_entries, req_id;
        node_prog::prog_type type;
        std::shared_ptr<node_prog::Node_State_Base> new_entry;
        acquire();
        assert(req_list.find(node_id) == req_list.end()); // state for this node definitely does not exist already
        req_list.emplace(node_id, std::unordered_set<uint64_t>());
        message::unpack_buffer(unpacker, num_entries);
        while (num_entries-- > 0) {
            message::unpack_buffer(unpacker, ptype);
            message::unpack_buffer(unpacker, req_id);
            type = (node_prog::prog_type)ptype;
            switch (type) {
                case node_prog::REACHABILITY: {
                    std::shared_ptr<node_prog::reach_node_state> rns(new node_prog::reach_node_state());
                    rns->unpack(unpacker);
                    new_entry = std::dynamic_pointer_cast<node_prog::Node_State_Base>(rns);
                    break;
                }
                case node_prog::DIJKSTRA: {
                    std::shared_ptr<node_prog::dijkstra_node_state> dns(new node_prog::dijkstra_node_state());
                    dns->unpack(unpacker);
                    new_entry = std::dynamic_pointer_cast<node_prog::Node_State_Base>(dns);
                    break;
                }
                case node_prog::CLUSTERING: {
                    std::shared_ptr<node_prog::clustering_node_state> cns(new node_prog::clustering_node_state());
                    cns->unpack(unpacker);
                    new_entry = std::dynamic_pointer_cast<node_prog::Node_State_Base>(cns);
                    break;
                }

                case node_prog::READ_NODE_PROPS: {
                    std::shared_ptr<node_prog::read_node_props_state> cns(new node_prog::read_node_props_state());
                    cns->unpack(unpacker);
                    new_entry = std::dynamic_pointer_cast<node_prog::Node_State_Base>(cns);
                    break;
                }

                case node_prog::READ_EDGES_PROPS: {
                    std::shared_ptr<node_prog::read_edges_props_state> cns(new node_prog::read_edges_props_state());
                    cns->unpack(unpacker);
                    new_entry = std::dynamic_pointer_cast<node_prog::Node_State_Base>(cns);
                    break;
                }

                case node_prog::READ_N_EDGES: {
                    std::shared_ptr<node_prog::read_n_edges_state> cns(new node_prog::read_n_edges_state());
                    cns->unpack(unpacker);
                    new_entry = std::dynamic_pointer_cast<node_prog::Node_State_Base>(cns);
                    break;
                }

                default:
                    WDEBUG << "Bad type in program state unpack " << type << std::endl;
            }
            req_map &rmap = prog_state.at(type);
            if (rmap.find(req_id) == rmap.end()) {
                rmap.emplace(req_id, std::shared_ptr<node_map>(new node_map()));
            }
            if (!rmap.at(req_id)->emplace(node_id, new_entry).second) {
                WDEBUG << "state already exists for node " << node_id << " and req id " << req_id << std::endl;
            }
            req_list.at(node_id).emplace(req_id);
        }
        release();
    }

    inline void
    program_state :: delete_node_state(uint64_t node_id)
    {
        acquire();
        if (req_list.find(node_id) != req_list.end()) {
            std::unordered_set<uint64_t> reqs = req_list.at(node_id);
            for (uint64_t req_id: reqs) {
                for (auto &t: prog_state) {
                    req_map &rmap = t.second;
                    if (rmap.find(req_id) != rmap.end()) {
                        rmap.at(req_id)->erase(node_id);
                        break;
                    }
                }
            }
        }
        req_list.erase(node_id);
        release();
    }

    inline void
    program_state :: done_requests(std::vector<std::pair<uint64_t, node_prog::prog_type>> &reqs)
    {
        acquire();
        for (auto &p: reqs) {
            uint64_t req_id = p.first;
            node_prog::prog_type type = p.second;
            done_ids.emplace(req_id);
            req_map &rmap = prog_state.at(type);
            if (rmap.find(req_id) != rmap.end()) {
                for (auto &p: *rmap.at(req_id)) {
                    uint64_t node_id = p.first;
                    req_list.at(node_id).erase(req_id);
                    if (req_list.at(node_id).empty()) {
                        req_list.erase(node_id);
                    }
                }
                rmap.erase(req_id);
            }
        }
        release();
    }

    inline bool
    program_state :: check_done_nolock(uint64_t req_id)
    {
        return (done_ids.find(req_id) != done_ids.end());
    }
            
    inline bool
    program_state :: check_done_request(uint64_t req_id)
    {
        bool ret;
        acquire();
        ret = check_done_nolock(req_id);
        release();
        return ret;
    }
}

#endif
