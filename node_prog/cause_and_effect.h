/*
 * ===============================================================
 *    Description:  Discover all paths between two vertices
 *                  predicated on max path len, node properties,
 *                  edge properties.
 *
 *         Author:  Ayush Dubey, dubey@cs.cornell.edu
 *
 * Copyright (C) 2014, Cornell University, see the LICENSE file
 *                     for licensing agreement
 * ===============================================================
 */

#ifndef weaver_node_prog_cause_and_effect_h_
#define weaver_node_prog_cause_and_effect_h_

#include <string>

#include "common/property_predicate.h"
#include "db/remote_node.h"
#include "node_prog/node.h"
#include "node_prog/edge.h"
#include "node_prog/base_classes.h"
#include "node_prog/cache_response.h"

namespace node_prog
{
    using edge_set = std::unordered_set<cl::edge, cl::hash_edge, cl::equals_edge>;

    struct cause_and_effect_params: public virtual Node_Parameters_Base
    {
        node_handle_t dest;
        uint32_t path_len;
        std::vector<predicate::prop_predicate> node_preds;
        std::vector<predicate::prop_predicate> edge_preds;
        std::unordered_map<node_handle_t, std::vector<cl::edge>> paths;

        bool returning;
        db::remote_node prev_node;
        node_handle_t src;
        std::unordered_set<node_handle_t> path_ancestors; // prevent cycles

        cause_and_effect_params();
        ~cause_and_effect_params() { }
        uint64_t size() const;
        void pack(e::packer&) const;
        void unpack(e::unpacker&);

        // no caching
        bool search_cache() { return false; }
        cache_key_t cache_key() { return cache_key_t(); }
    };

    struct cdp_len_state: public virtual Node_State_Base
    {
        uint32_t outstanding_count;
        std::vector<db::remote_node> prev_nodes;
        std::unordered_map<node_handle_t, edge_set> paths;

        cdp_len_state();
        ~cdp_len_state() { }
        uint64_t size() const;
        void pack(e::packer&) const;
        void unpack(e::unpacker&);
    };

    struct cause_and_effect_state: public virtual Node_State_Base
    {
        std::unordered_map<uint32_t, cdp_len_state> vmap;
        uint32_t max_path_len;

        cause_and_effect_state();
        ~cause_and_effect_state() { }
        uint64_t size() const;
        void pack(e::packer&) const;
        void unpack(e::unpacker&);
    };

   std::pair<search_type, std::vector<std::pair<db::remote_node, cause_and_effect_params>>>
   cause_and_effect_node_program(node &n,
       db::remote_node &rn,
       cause_and_effect_params &params,
       std::function<cause_and_effect_state&()> state_getter,
       std::function<void(std::shared_ptr<Cache_Value_Base>, std::shared_ptr<std::vector<db::remote_node>>, cache_key_t)>&,
       cache_response<Cache_Value_Base>*);
}

#endif
