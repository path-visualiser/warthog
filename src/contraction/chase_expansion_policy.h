#ifndef WARTHOG_CHASE_EXPANSION_POLICY_H
#define WARTHOG_CHASE_EXPANSION_POLICY_H

// contraction/chase_expansion_policy.h
//
// CHASE is a two-stage variant of bi-directional contraction hierarchies
// plus arc flags. Edges are pruned only in the second stage.
//
// Switching phases:
//      - Each time generate_target_node or generate_start_node is called
//        the policy switches to phase1 (i.e. successors are not pruned)
//      - Each time begin_phase2 is called the policy switches to phase2
//        and attempts to prune every candidate successor before generating
// 
// For theoretical details on CHASE see:
//
// [Bauer, Delling, Sanders, Schieferdecker, Schultes and Wagner, 
// Combining Hierarchical and Goal-directed Speed-up Techniques 
// for Dijkstra's Algorithm, Journal of Experimental Algorithms,
// vol 15, 2010]
//
// @author: dharabor
// @created: 2017-10-11
//

#include "contraction.h"
#include "expansion_policy.h"
#include "planar_graph.h"

#include <vector>

namespace warthog{

class af_filter;
class problem_instance;
class search_node;

class chase_expansion_policy : public  expansion_policy
{
    public:
        // @param backward: when true successors are generated by following 
        // incoming arcs rather than outgoing arcs (default is outgoing)
        //
        // @param filter: the arc-flags filter used to prune arcs
        //
        chase_expansion_policy(warthog::graph::planar_graph* g, 
                warthog::af_filter* filter,
                bool backward=false);

        virtual 
        ~chase_expansion_policy() { }

		virtual void 
		expand(warthog::search_node*, warthog::problem_instance*);

        virtual void
        get_xy(uint32_t node_id, int32_t& x, int32_t& y);

        virtual warthog::search_node* 
        generate_start_node(warthog::problem_instance* pi);

        virtual warthog::search_node*
        generate_target_node(warthog::problem_instance* pi);

        inline void
        begin_phase2()
        {
            fn_filter_arc = 
                &warthog::chase_expansion_policy::phase2_filter_fn;
        }

        virtual size_t
        mem();

        inline uint32_t
        get_num_nodes() { return g_->get_num_nodes(); }

    private:
        bool backward_;
        warthog::graph::planar_graph* g_;
        warthog::af_filter* filter_;

        // we use dynamically assigned function pointers to 
        // select the right set of successors during expansion
        // (outgoing up successors for the forward direction and 
        // incoming down successors for the backward direction)
        typedef warthog::graph::edge_iter
                (warthog::chase_expansion_policy::*chep_get_iter_fn) 
                (warthog::graph::node* n);
        chep_get_iter_fn fn_begin_iter_;
        chep_get_iter_fn fn_end_iter_;

        inline warthog::graph::edge_iter
        get_fwd_begin_iter(warthog::graph::node* n) 
        { return n->outgoing_begin(); }

        inline warthog::graph::edge_iter
        get_fwd_end_iter(warthog::graph::node* n) 
        { return n->outgoing_end(); }

        inline warthog::graph::edge_iter
        get_bwd_begin_iter(warthog::graph::node* n) 
        { return n->incoming_begin(); }

        inline warthog::graph::edge_iter
        get_bwd_end_iter(warthog::graph::node* n) 
        { return n->incoming_end(); }

        // we also use dynamically assigned function pointers to pick
        // the right filtering function. during the first stage of 
        // CHASE, nothing is pruned. during the second stage, we prune
        // nodes using arc-flasgs
        typedef bool(warthog::chase_expansion_policy::*filter_fn)(
                uint32_t node_id, uint32_t edge_idx);

        filter_fn fn_filter_arc;

        // nothing is filtered in phase1
        bool 
        phase1_filter_fn(uint32_t node_id, uint32_t edge_idx) 
        {
            return false;
        }   
        
        // apply arcflags filtering in phase2
        bool
        phase2_filter_fn(uint32_t node_id, uint32_t edge_idx)
        {
            return filter_->filter(node_id, edge_idx);
        }

};

}
#endif

