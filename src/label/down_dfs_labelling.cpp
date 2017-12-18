#include "down_dfs_labelling.h"
#include "planar_graph.h"

#include "search_node.h"
#include "zero_heuristic.h"
#include "flexible_astar.h"
#include "problem_instance.h"
#include "solution.h"
#include "graph_expansion_policy.h"

#include <algorithm>

warthog::label::down_dfs_labelling::down_dfs_labelling(
        warthog::graph::planar_graph* g, 
        std::vector<uint32_t>* partitioning)
{
    dfs_order_ = new std::vector< int32_t >();
    lab_ = new std::vector< std::vector < down_dfs_label > >();

    g_ = g;
    part_ = partitioning ;

    // figure out how many bytes are required per label
    uint32_t max_id = *(std::max_element(part_->begin(), part_->end()));
    bytes_per_af_label_ = (max_id / 8) + !!(max_id % 8);
}

warthog::label::down_dfs_labelling::~down_dfs_labelling()
{ 
    delete lab_;
    delete dfs_order_;
}

void
warthog::label::down_dfs_labelling::compute_labels(
        std::vector<uint32_t>* rank)
{
    // allocate some memory for id-range labels
    dfs_order_->resize(this->g_->get_num_nodes(), INT32_MAX);
    lab_->resize(this->g_->get_num_nodes());

    // find the apex of the hierarchy
    uint32_t apex_id = 0;
    for(uint32_t i = 0; i < rank->size(); i++)
    { 
        if(rank->at(i) > rank->at(apex_id)) 
        { apex_id = i; } 
    }

    // traverse the graph and compute node and edge labels using DFS postorder
    std::vector< down_dfs_label > node_labels(
            this->g_->get_num_nodes(), down_dfs_label(bytes_per_af_label_));

    {
        down_dfs_label dummy(bytes_per_af_label_);
        for(uint32_t n_id = 0; n_id < this->g_->get_num_nodes(); n_id++)
        {
            warthog::graph::node* n = this->g_->get_node(n_id);
            lab_->at(n_id).resize(n->out_degree(), dummy);
        }
    }

    // down labels
    uint32_t dfs_id = 0;
    std::function<void(uint32_t)> label_fn = 
        [this, rank, &node_labels, &apex_id, &dfs_id, &label_fn] 
        (uint32_t source_id) -> void
        {
            down_dfs_label& s_lab = node_labels.at(source_id);
            warthog::graph::node* source = this->g_->get_node(source_id);
            warthog::graph::edge_iter begin = source->outgoing_begin();

            for( warthog::graph::edge_iter it = begin; 
                    it != source->outgoing_end();
                    it++)
            {
                // skip up edges
                if(rank->at(it->node_id_) > rank->at(source_id)) 
                { continue; }

                // DFS
                if(dfs_order_->at(it->node_id_) == INT32_MAX)
                { label_fn(it->node_id_); }

                // label the edge
                lab_->at(source_id).at(it - begin) = 
                    node_labels.at(it->node_id_);

                // update the range down-reachable from source_id
                s_lab.merge(node_labels.at(it->node_id_));
                assert(s_lab.ids_.contains(
                            this->dfs_order_->at(it->node_id_)));
            }

            if(dfs_order_->at(source_id) == INT32_MAX)
            { this->dfs_order_->at(source_id) = dfs_id++; }

            s_lab.rank_.grow(rank->at(source_id));

            s_lab.ids_.grow(this->dfs_order_->at(source_id));

            int32_t x, y;
            this->g_->get_xy(source_id, x, y);
            s_lab.bbox_.grow(x, y);

            uint32_t s_part = this->part_->at(source_id);
            s_lab.flags_[s_part >> 3] |= (1 << (s_part & 7)); // div8, mod8
            assert(s_lab.flags_[s_part >> 3] & (1 << (s_part & 7)));
        };
    label_fn(apex_id);

    // compute up-closure apex for every node
    std::vector< int32_t > up_apex(this->g_->get_num_nodes(), INT32_MAX);
    std::function<void(uint32_t)> up_label_fn = 
        [this, rank, &node_labels, &up_apex, &up_label_fn] 
        (uint32_t source_id) -> void
        {
            warthog::graph::node* source = this->g_->get_node(source_id);
            warthog::graph::edge_iter begin = source->outgoing_begin();
            warthog::graph::edge_iter end = source->outgoing_end();

            // compute a label for the up-closure 
            uint32_t apex_id = source_id;
            for( warthog::graph::edge_iter it = begin; it != end; it++)
            {
                // skip down edges
                if(rank->at(it->node_id_) < rank->at(source_id)) 
                { continue; }

                // DFS
                if(up_apex.at(it->node_id_) == INT32_MAX)
                { up_label_fn(it->node_id_); }

                // update the up-closure label
                uint32_t succ_apex_id = up_apex.at(it->node_id_);
                if(rank->at(succ_apex_id) > rank->at(apex_id))
                { apex_id = succ_apex_id; }
            }
            up_apex.at(source_id) = apex_id;

            // compute labels for each edge (a, b) where a < b in the CH.
            // to compute the label we take the apex node in the up-closure 
            // from node a and compute its down-closure. 
            for( warthog::graph::edge_iter it = begin; it != end; it++)
            {
                // down edges of n are already labeled, so we can skip them
                if(rank->at(it->node_id_) < rank->at(source_id)) { continue; }
                
                // up-closure part of the label
                down_dfs_label& e_lab = lab_->at(source_id).at(it - begin);
                e_lab.merge(node_labels.at(apex_id));
                assert(e_lab.ids_.contains(apex_id));
            }
        };

    for(uint32_t n_id = 0; n_id < this->g_->get_num_nodes(); n_id++)
    { 
        up_label_fn(n_id); 
    }
}

void
warthog::label::down_dfs_labelling::improve_labels(
        std::vector<uint32_t>* rank)
{
    // select some nodes and reset their labels
    std::vector<uint32_t> sources;
    for(uint32_t i = 0; i < this->g_->get_num_nodes(); i++)
    {
        // nodes with degree >= 100
        warthog::graph::node* n = this->g_->get_node(i);
        if(n->out_degree() >= 100)
        // top 1% highest nodes
        //if(rank->at(i) >= (uint32_t)(rank->size()*0.99))
        { 
            sources.push_back(i); 
            warthog::graph::edge_iter begin = n->outgoing_begin();
            warthog::graph::edge_iter end = n->outgoing_end();
            for(warthog::graph::edge_iter it = begin; it != end; it++)
            {
                this->lab_->at(i).at(it - begin) = 
                    down_dfs_label(this->bytes_per_af_label_);
            }
        }
    }
    
    // alocate memory for the first moves
    std::vector<uint32_t> first_move(g_->get_num_nodes());

    uint32_t source_id;

    // callback function used to record the optimal first move 
    std::function<void(warthog::search_node*, warthog::search_node*,
            double, uint32_t)> on_generate_fn = 
    [this, &source_id, &first_move]
    (warthog::search_node* succ, warthog::search_node* from,
                double edge_cost, uint32_t edge_id) -> void
    {
        if(from == 0) { return; } // start node 

        if(from->get_id() == source_id) // start node successors
        { 
            assert(edge_id < this->g_->get_node(source_id)->out_degree());
            first_move.at(succ->get_id()) = edge_id; 
        }
        else // all other nodes
        {
            uint32_t s_id = succ->get_id();
            uint32_t f_id = from->get_id();
            double alt_g = from->get_g() + edge_cost;
            double g_val = succ->get_search_id() == from->get_search_id() 
                                ? succ->get_g() : warthog::INF; 

            assert(first_move.at(f_id) < 
                    this->g_->get_node(source_id)->out_degree());

            //  update first move
            if(alt_g < g_val) 
            { first_move.at(s_id) = first_move.at(f_id); }
        }
    };


    std::function<void(warthog::search_node*)> on_expand_fn =
        [this, &source_id, &rank, &first_move] (warthog::search_node* current) -> void
        {
            if(current->get_id() == source_id) { return; }

            uint32_t node_id = current->get_id();
            uint32_t edge_idx = first_move.at(node_id);
            assert(edge_idx < this->lab_->at(source_id).size());

            down_dfs_label& s_lab = this->lab_->at(source_id).at(edge_idx);

            s_lab.rank_.grow(rank->at(node_id));
            s_lab.ids_.grow(this->dfs_order_->at(node_id));

            int32_t x, y;
            this->g_->get_xy(node_id, x, y);
            s_lab.bbox_.grow(x, y);

            uint32_t s_part = this->part_->at(node_id);
            s_lab.flags_[s_part >> 3] |= (1 << (s_part & 7)); // div8, mod8
            assert(s_lab.flags_[s_part >> 3] & (1 << (s_part & 7)));
        };

    warthog::zero_heuristic h;
    warthog::fch_expansion_policy exp(g_, rank);
    warthog::flexible_astar 
        <warthog::zero_heuristic, warthog::fch_expansion_policy>
            dijk(&h, &exp);
    dijk.apply_on_generate(on_generate_fn);
    dijk.apply_on_expand(on_expand_fn);

    // bla bla store for every node the first move id
    // and grow the label on expand
    std::cerr << "improving labels for " << sources.size() << " nodes\n";
    std::cerr << "progress [";
    for(uint32_t i = 0; i<100; i++) { std::cerr << " "; }
    std::cerr << "]\rprogress [";
    uint32_t progress = 0;
    uint32_t one_pct = (sources.size() / 100)+1;
    for(auto i :  sources)
    {
        source_id = i;
        uint32_t ext_source_id = g_->to_external_id(source_id);
        warthog::problem_instance problem(ext_source_id, warthog::INF);
        //problem.verbose_ = true;
        warthog::solution sol;
        dijk.get_path(problem, sol);
        if((progress % one_pct) == 0)
        {
            std::cerr << "=";
        }
        progress++;
    }
}
